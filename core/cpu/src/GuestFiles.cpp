#include <cpu/GuestFiles.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Cpu {
namespace {

// Host transfers move at most this much per syscall, so a guest transfer of any size needs
// only one bounded host buffer.
constexpr std::uint64_t chunkSize = 16 * 1024 * 1024;
constexpr std::uint64_t maxCount = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::size_t maxPath = 1024;
constexpr std::size_t maxComponent = 255;
constexpr std::size_t maxDescriptors = 1024;

// FreeBSD open(2) flags as the guest passes them.
constexpr std::uint32_t guestAccessMode = 0x3, guestWriteOnly = 0x1, guestReadWrite = 0x2;
constexpr std::uint32_t guestAppend = 0x8, guestCreate = 0x200, guestTruncate = 0x400, guestExclusive = 0x800,
                        guestDirectory = 0x20000;
// Hints with no effect on the bytes a guest sees: O_NONBLOCK, O_SHLOCK, O_EXLOCK, O_ASYNC, O_FSYNC,
// O_NOFOLLOW (every lookup is already no-follow), O_DSYNC, O_NOCTTY, O_DIRECT and O_CLOEXEC.
constexpr std::uint32_t guestIgnored = 0x4 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x1000 | 0x8000 | 0x10000 | 0x100000;
constexpr std::uint32_t guestKnown = guestAccessMode | guestAppend | guestCreate | guestTruncate | guestExclusive |
                                     guestDirectory | guestIgnored;

enum GuestErrno : unsigned {
    NotPermitted = 1, NoEntry = 2, BadDescriptor = 9, Access = 13, Fault = 14, Busy = 16, Exists = 17, CrossDevice = 18,
    NotDirectory = 20, IsDirectory = 21, Invalid = 22, TooMany = 24, ReadOnly = 30, NameTooLong = 63,
};

std::int64_t kernelError(unsigned guestError) {
    return std::bit_cast<std::int32_t>(0x80020000u | guestError);
}

std::int64_t nativeError(int error) {
    switch (error) {
    case EPERM: return kernelError(1);
    case ENOENT: return kernelError(2);
    case EINTR: return kernelError(4);
    case EIO: return kernelError(5);
    case ENXIO: return kernelError(6);
    case E2BIG: return kernelError(7);
    case EBADF: return kernelError(9);
    case ENOMEM: return kernelError(12);
    case EACCES: return kernelError(13);
    case EFAULT: return kernelError(14);
    case EBUSY: return kernelError(16);
    case EEXIST: return kernelError(17);
    case EXDEV: return kernelError(18);
    case ENODEV: return kernelError(19);
    case ENOTDIR: return kernelError(20);
    case EISDIR: return kernelError(21);
    case EINVAL: return kernelError(22);
    case ENFILE: return kernelError(23);
    case EMFILE: return kernelError(24);
    case ETXTBSY: return kernelError(26);
    case EFBIG: return kernelError(27);
    case ENOSPC: return kernelError(28);
    case ESPIPE: return kernelError(29);
    case EROFS: return kernelError(30);
    case EMLINK: return kernelError(31);
    case EAGAIN: return kernelError(35);
    case EOPNOTSUPP: return kernelError(45);
#if ENOTSUP != EOPNOTSUPP
    case ENOTSUP: return kernelError(45);
#endif
    case ELOOP: return kernelError(62);
    case ENAMETOOLONG: return kernelError(63);
    case ENOTEMPTY: return kernelError(66);
    case EDQUOT: return kernelError(69);
    case EOVERFLOW: return kernelError(84);
    default: return kernelError(5);
    }
}

struct Descriptor {
    int value = -1;
    explicit Descriptor(int descriptor = -1) : value(descriptor) {}
    ~Descriptor() { if (value >= 0) ::close(value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (value >= 0) ::close(value);
        value = std::exchange(other.value, -1);
        return *this;
    }
};

struct DirectoryEntry {
    std::string name;
    std::uint32_t fileno = 0;
    std::uint8_t type = 0;
};

struct OpenFile {
    Descriptor file;
    bool directory = false;
    // Directory position: an index into a snapshot taken on the first read after open or rewind.
    std::optional<std::vector<DirectoryEntry>> entries;
    std::size_t cursor = 0;
};

struct Mount {
    const char* name;
    bool writable;
    std::filesystem::path host;  // empty: not configured
    Descriptor root;
};

// A resolved guest path: the directory that holds the final component, opened without following
// any symlink, plus that component. An empty leaf names the mount root itself.
struct Location {
    Mount* mount = nullptr;
    Descriptor owned;
    int parent = -1;
    std::string leaf;
    // The path ends in "/" or "/.": the final component must be a directory.
    bool trailingSlash = false;
    bool finalDot = false;
};

template<class T> void put(std::array<std::byte, GuestFiles::StatSize>& output, std::size_t offset, T value) {
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    std::copy(bytes.begin(), bytes.end(), output.begin() + static_cast<std::ptrdiff_t>(offset));
}

void putTime(std::array<std::byte, GuestFiles::StatSize>& output, std::size_t offset, const timespec& time) {
    put<std::int64_t>(output, offset, static_cast<std::int64_t>(time.tv_sec));
    put<std::int64_t>(output, offset + 8, static_cast<std::int64_t>(time.tv_nsec));
}

// FreeBSD struct stat as libkernel returns it (120 bytes).
std::array<std::byte, GuestFiles::StatSize> guestStat(const struct stat& status) {
    std::array<std::byte, GuestFiles::StatSize> output{};
    put<std::uint32_t>(output, 0, static_cast<std::uint32_t>(status.st_dev));
    put<std::uint32_t>(output, 4, static_cast<std::uint32_t>(status.st_ino));
    put<std::uint16_t>(output, 8, static_cast<std::uint16_t>(status.st_mode));
    put<std::uint16_t>(output, 10, static_cast<std::uint16_t>(status.st_nlink));
    put<std::uint32_t>(output, 12, static_cast<std::uint32_t>(status.st_uid));
    put<std::uint32_t>(output, 16, static_cast<std::uint32_t>(status.st_gid));
    put<std::uint32_t>(output, 20, static_cast<std::uint32_t>(status.st_rdev));
#if defined(__APPLE__)
    putTime(output, 24, status.st_atimespec);
    putTime(output, 40, status.st_mtimespec);
    putTime(output, 56, status.st_ctimespec);
    putTime(output, 104, status.st_birthtimespec);
    put<std::uint32_t>(output, 92, static_cast<std::uint32_t>(status.st_flags));
    put<std::uint32_t>(output, 96, static_cast<std::uint32_t>(status.st_gen));
#else
    putTime(output, 24, status.st_atim);
    putTime(output, 40, status.st_mtim);
    putTime(output, 56, status.st_ctim);
    putTime(output, 104, status.st_ctim);
#endif
    put<std::int64_t>(output, 72, static_cast<std::int64_t>(status.st_size));
    put<std::int64_t>(output, 80, static_cast<std::int64_t>(status.st_blocks));
    put<std::uint32_t>(output, 88, static_cast<std::uint32_t>(status.st_blksize));
    return output;
}

// True when the two existing-or-not paths are the same directory or child lies below parent.
bool within(const std::filesystem::path& child, const std::filesystem::path& parent) {
    std::error_code error;
    const auto left = std::filesystem::weakly_canonical(std::filesystem::absolute(child, error), error);
    const auto right = std::filesystem::weakly_canonical(std::filesystem::absolute(parent, error), error);
    auto inner = left.begin();
    for (auto outer = right.begin(); outer != right.end(); ++outer, ++inner) {
        if (outer->empty() && std::next(outer) == right.end()) break;  // trailing separator
        if (inner == left.end() || *inner != *outer) return false;
    }
    return true;
}

}

struct GuestFiles::Impl {
    Machine& machine;
    std::array<Mount, 4> mounts;
    std::unordered_map<std::int32_t, OpenFile> descriptors;
    std::int32_t nextDescriptor = 3;
    // One host bounce buffer, grown to at most one chunk and reused by every transfer.
    std::vector<std::byte> transfer;

    std::span<std::byte> transferBuffer(std::uint64_t count) {
        const auto size = static_cast<std::size_t>(std::min(count, chunkSize));
        if (transfer.size() < size) transfer.resize(size);
        return std::span(transfer).first(size);
    }

    Impl(Machine& guest, const std::filesystem::path& resourceRoot, const GuestFilesOptions& options) : machine(guest),
        mounts{Mount{"app0", false, resourceRoot, Descriptor{}},
               Mount{"temp0", true, {}, Descriptor{}},
               Mount{"download0", true, {}, Descriptor{}},
               Mount{"savedata0", true, {}, Descriptor{}}} {
        mounts[0].root = Descriptor(::open(resourceRoot.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (mounts[0].root.value < 0) throw std::runtime_error("Cannot open guest /app0 resource root: native errno " + std::to_string(errno));
        if (!options.TitleDataRoot.empty()) {
            if (Overlaps(options.TitleDataRoot, resourceRoot))
                throw std::runtime_error("Guest title data directory must not be inside the resource root (the game dump is read-only), "
                                         "and the resource root must not be inside a writable mount");
            for (auto& mount : mounts) if (mount.writable) mount.host = options.TitleDataRoot / mount.name;
        }
    }

    std::int64_t readPath(std::uint64_t address, std::string& path) const {
        for (std::size_t index = 0; index < maxPath; ++index) {
            if (address >= std::numeric_limits<std::uint64_t>::max() - index) return kernelError(Fault);
            std::array<std::byte, 1> byte;
            try {
                machine.CheckAccess(address + index, 1, Permission::Read);
                machine.Read(address + index, byte);
            } catch (const std::runtime_error&) { return kernelError(Fault); }
            if (byte[0] == std::byte{0}) return 0;
            path.push_back(static_cast<char>(std::to_integer<unsigned char>(byte[0])));
        }
        return kernelError(NameTooLong);
    }

    // Writable mount roots are created under the title data directory by the first operation
    // that writes there; until then a query of the mount sees ENOENT and creates nothing.
    std::int64_t mountRoot(Mount& mount, bool create) {
        if (mount.root.value >= 0) return 0;
        if (mount.host.empty()) return kernelError(NoEntry);
        std::error_code error;
        if (create) std::filesystem::create_directories(mount.host, error);
        Descriptor root(::open(mount.host.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (root.value < 0) return nativeError(errno);
        mount.root = std::move(root);
        return 0;
    }

    // Walks the guest path one component at a time from the mount root with O_NOFOLLOW, so no
    // symlink is ever followed and ".." is refused: nothing outside a mount root is reachable.
    std::int64_t resolve(std::uint64_t address, Location& location, bool create = false) {
        std::string path;
        if (const auto error = readPath(address, path)) return error;
        if (path.empty() || path.front() != '/') return kernelError(NoEntry);
        location.finalDot = path.ends_with("/.");
        location.trailingSlash = path.back() == '/' || location.finalDot;
        std::vector<std::string> components;
        for (std::size_t begin = 0; begin <= path.size();) {
            const auto end = std::min(path.find('/', begin), path.size());
            auto component = path.substr(begin, end - begin);
            begin = end + 1;
            if (component.empty() || component == ".") continue;
            if (component == "..") return kernelError(Access);
            if (component.size() > maxComponent) return kernelError(NameTooLong);
            components.push_back(std::move(component));
        }
        if (components.empty()) return kernelError(NoEntry);
        const auto mount = std::find_if(mounts.begin(), mounts.end(), [&](const Mount& value) { return components.front() == value.name; });
        if (mount == mounts.end()) return kernelError(NoEntry);
        if (const auto error = mountRoot(*mount, create && mount->writable)) return error;
        location.mount = &*mount;
        location.parent = mount->root.value;
        for (std::size_t index = 1; index + 1 < components.size(); ++index) {
            Descriptor child(::openat(location.parent, components[index].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (child.value < 0) return nativeError(errno);
            location.owned = std::move(child);
            location.parent = location.owned.value;
        }
        if (components.size() > 1) location.leaf = components.back();
        return 0;
    }

    std::int64_t status(Location& location, struct stat& output) const {
        const int result = location.leaf.empty() ? ::fstat(location.parent, &output) :
            ::fstatat(location.parent, location.leaf.c_str(), &output, AT_SYMLINK_NOFOLLOW);
        if (result < 0) return nativeError(errno);
        if (location.trailingSlash && !S_ISDIR(output.st_mode)) return kernelError(NotDirectory);
        return 0;
    }

    std::int64_t storeStatus(const struct stat& value, std::uint64_t address) {
        if (address > std::numeric_limits<std::uint64_t>::max() - StatSize) return kernelError(Fault);
        try { machine.CheckAccess(address, StatSize, Permission::Write); }
        catch (const std::runtime_error&) { return kernelError(Fault); }
        machine.Write(address, guestStat(value));
        return 0;
    }

    OpenFile* find(std::int32_t descriptor) {
        const auto found = descriptors.find(descriptor);
        return found == descriptors.end() ? nullptr : &found->second;
    }

    std::int64_t guestRange(std::uint64_t address, std::uint64_t count, Permission permission) const {
        if (count > std::numeric_limits<std::uint64_t>::max() - address) return kernelError(Fault);
        try { machine.CheckAccess(address, static_cast<std::size_t>(count), permission); }
        catch (const std::runtime_error&) { return kernelError(Fault); }
        return 0;
    }

    std::int64_t read(std::int32_t descriptor, std::uint64_t destination, std::uint64_t count,
                      bool positional, std::int64_t offset) {
        auto* file = find(descriptor);
        if (!file) return kernelError(BadDescriptor);
        if (file->directory) return kernelError(IsDirectory);
        if (count > maxCount || (positional && offset < 0)) return kernelError(Invalid);
        if (!count) return 0;
        if (const auto error = guestRange(destination, count, Permission::Write)) return error;
        if (positional) count = std::min(count, maxCount - static_cast<std::uint64_t>(offset));
        const auto buffer = transferBuffer(count);
        std::uint64_t done = 0;
        while (done < count) {
            const auto size = static_cast<std::size_t>(std::min(chunkSize, count - done));
            const auto result = positional ?
                ::pread(file->file.value, buffer.data(), size, static_cast<off_t>(offset + static_cast<std::int64_t>(done))) :
                ::read(file->file.value, buffer.data(), size);
            if (result < 0) {
                if (errno == EINTR) continue;
                if (done) break;
                return nativeError(errno);
            }
            if (!result) break;
            machine.Write(destination + done, buffer.first(static_cast<std::size_t>(result)));
            done += static_cast<std::uint64_t>(result);
            if (static_cast<std::size_t>(result) < size) break;
        }
        return static_cast<std::int64_t>(done);
    }

    std::int64_t write(std::int32_t descriptor, std::uint64_t source, std::uint64_t count,
                       bool positional, std::int64_t offset) {
        auto* file = find(descriptor);
        if (!file) return kernelError(BadDescriptor);
        if (count > maxCount || (positional && offset < 0)) return kernelError(Invalid);
        if (positional && count > maxCount - static_cast<std::uint64_t>(offset)) return kernelError(Invalid);
        if (!count) return 0;
        if (const auto error = guestRange(source, count, Permission::Read)) return error;
        const auto buffer = transferBuffer(count);
        std::uint64_t done = 0;
        while (done < count) {
            const auto size = static_cast<std::size_t>(std::min(chunkSize, count - done));
            machine.Read(source + done, buffer.first(size));
            std::size_t written = 0;
            while (written < size) {
                const auto result = positional ?
                    ::pwrite(file->file.value, buffer.data() + written, size - written,
                             static_cast<off_t>(offset + static_cast<std::int64_t>(done + written))) :
                    ::write(file->file.value, buffer.data() + written, size - written);
                if (result < 0) {
                    if (errno == EINTR) continue;
                    if (done + written) return static_cast<std::int64_t>(done + written);
                    return nativeError(errno);
                }
                if (!result) return static_cast<std::int64_t>(done + written);
                written += static_cast<std::size_t>(result);
            }
            done += size;
        }
        return static_cast<std::int64_t>(done);
    }

    std::int64_t list(OpenFile& file) {
        Descriptor copy(::openat(file.file.value, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (copy.value < 0) return nativeError(errno);
        DIR* stream = ::fdopendir(copy.value);
        if (!stream) return nativeError(errno);
        copy.value = -1;  // owned by the stream now
        std::vector<DirectoryEntry> entries;
        errno = 0;
        while (const auto* entry = ::readdir(stream)) {
            const std::string name(entry->d_name);
            if (name.empty() || name.size() > maxComponent) continue;
            entries.push_back({name, static_cast<std::uint32_t>(entry->d_ino), static_cast<std::uint8_t>(entry->d_type)});
        }
        const int error = errno;
        ::closedir(stream);
        if (error) return nativeError(error);
        file.entries = std::move(entries);
        return 0;
    }

    std::int64_t directoryEntries(std::int32_t descriptor, std::uint64_t buffer, std::uint64_t count, std::uint64_t position) {
        auto* file = find(descriptor);
        if (!file) return kernelError(BadDescriptor);
        if (!file->directory) return kernelError(Invalid);
        if (count > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) return kernelError(Invalid);
        if (const auto error = guestRange(buffer, count, Permission::Write)) return error;
        if (position) if (const auto error = guestRange(position, 8, Permission::Write)) return error;
        if (!file->entries) if (const auto error = list(*file)) return error;
        const auto start = file->cursor;
        std::vector<std::byte> output;
        while (file->cursor < file->entries->size()) {
            const auto& entry = (*file->entries)[file->cursor];
            const auto record = (8 + entry.name.size() + 1 + 3) & ~std::size_t{3};
            if (output.size() + record > count) break;
            const auto offset = output.size();
            output.resize(offset + record);
            const auto reclen = static_cast<std::uint16_t>(record);
            std::memcpy(output.data() + offset, &entry.fileno, 4);
            std::memcpy(output.data() + offset + 4, &reclen, 2);
            output[offset + 6] = static_cast<std::byte>(entry.type);
            output[offset + 7] = static_cast<std::byte>(entry.name.size());
            std::memcpy(output.data() + offset + 8, entry.name.data(), entry.name.size());
            ++file->cursor;
        }
        if (output.empty() && file->cursor < file->entries->size()) return kernelError(Invalid);
        if (!output.empty()) machine.Write(buffer, output);
        if (position) {
            const auto value = static_cast<std::int64_t>(start);
            machine.Write(position, std::as_bytes(std::span(&value, 1)));
        }
        return static_cast<std::int64_t>(output.size());
    }

    std::int64_t open(std::uint64_t address, std::uint32_t flags) {
        const auto access = flags & guestAccessMode;
        if ((flags & ~guestKnown) || access == guestAccessMode) return kernelError(Invalid);
        Location location;
        if (const auto error = resolve(address, location, access != 0 || (flags & (guestCreate | guestTruncate)))) return error;
        if (descriptors.size() >= maxDescriptors || nextDescriptor == std::numeric_limits<std::int32_t>::max())
            return kernelError(TooMany);
        const bool writes = access != 0 || (flags & guestTruncate);
        const bool create = flags & guestCreate, exclusive = flags & guestExclusive;
        if (!location.mount->writable && writes) return kernelError(ReadOnly);
        Descriptor file;
        if (location.leaf.empty()) {
            if (create && exclusive) return kernelError(Exists);
            if (writes) return kernelError(IsDirectory);
            file = Descriptor(::openat(location.parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        } else {
            int native = (access == guestWriteOnly ? O_WRONLY : access == guestReadWrite ? O_RDWR : O_RDONLY) |
                         O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
            if ((flags & guestDirectory) || location.trailingSlash) native |= O_DIRECTORY;
            if (location.mount->writable) {
                if (flags & guestAppend) native |= O_APPEND;
                if (create) native |= O_CREAT;
                if (flags & guestTruncate) native |= O_TRUNC;
                if (exclusive) native |= O_EXCL;
            } else if (create) {
                // Never create in a read-only mount: an existing file opens, a missing one is EROFS.
                struct stat existing {};
                if (::fstatat(location.parent, location.leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) < 0)
                    return errno == ENOENT ? kernelError(ReadOnly) : nativeError(errno);
                if (exclusive) return kernelError(Exists);
            }
            // Guest permission bits are not applied: a title's own files must stay readable to it.
            file = Descriptor(::openat(location.parent, location.leaf.c_str(), native, 0666));
        }
        if (file.value < 0) return nativeError(errno);
        struct stat status {};
        if (::fstat(file.value, &status) < 0) return nativeError(errno);
        const bool directory = S_ISDIR(status.st_mode);
        if (!directory && !S_ISREG(status.st_mode)) return kernelError(Access);
        const auto descriptor = nextDescriptor++;
        OpenFile opened;
        opened.file = std::move(file);
        opened.directory = directory;
        descriptors.emplace(descriptor, std::move(opened));
        return descriptor;
    }

    // A path written with a trailing "/" or "/." names a directory: an existing non-directory is ENOTDIR.
    std::int64_t requireDirectory(const Location& location) const {
        if (!location.trailingSlash || location.leaf.empty()) return 0;
        struct stat existing {};
        if (::fstatat(location.parent, location.leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISDIR(existing.st_mode))
            return kernelError(NotDirectory);
        return 0;
    }

    // Shared shape of the path mutations: resolve, answer for the mount root, refuse read-only
    // mounts, then run the host call.
    template<class Operation> std::int64_t mutate(std::uint64_t address, bool create, std::int64_t rootError,
                                                  Operation&& operation) {
        Location location;
        if (const auto error = resolve(address, location, create)) return error;
        if (location.leaf.empty()) return rootError;
        if (const auto error = requireDirectory(location)) return error;
        if (!location.mount->writable) {
            // Creating something that already exists reports EEXIST even on a read-only mount.
            struct stat existing {};
            if (create && ::fstatat(location.parent, location.leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
                return kernelError(Exists);
            return kernelError(ReadOnly);
        }
        return operation(location.parent, location.leaf.c_str()) < 0 ? nativeError(errno) : 0;
    }
};

bool GuestFiles::Overlaps(const std::filesystem::path& titleDataRoot, const std::filesystem::path& resourceRoot) {
    if (within(titleDataRoot, resourceRoot)) return true;
    for (const auto* mount : {"temp0", "download0", "savedata0"})
        if (within(resourceRoot, titleDataRoot / mount)) return true;
    return false;
}

GuestFiles::GuestFiles(Machine& machine, const std::filesystem::path& resourceRoot, GuestFilesOptions options) :
    impl(std::make_unique<Impl>(machine, resourceRoot, options)) {}
GuestFiles::~GuestFiles() = default;

std::int64_t GuestFiles::Open(std::uint64_t address, std::uint32_t flags, std::uint16_t) {
    return impl->open(address, flags);
}

std::int64_t GuestFiles::Read(std::int32_t descriptor, std::uint64_t destination, std::uint64_t count) {
    return impl->read(descriptor, destination, count, false, 0);
}

std::int64_t GuestFiles::Pread(std::int32_t descriptor, std::uint64_t destination, std::uint64_t count, std::int64_t offset) {
    return impl->read(descriptor, destination, count, true, offset);
}

std::int64_t GuestFiles::Write(std::int32_t descriptor, std::uint64_t source, std::uint64_t count) {
    return impl->write(descriptor, source, count, false, 0);
}

std::int64_t GuestFiles::Pwrite(std::int32_t descriptor, std::uint64_t source, std::uint64_t count, std::int64_t offset) {
    return impl->write(descriptor, source, count, true, offset);
}

std::int64_t GuestFiles::Lseek(std::int32_t descriptor, std::int64_t offset, std::int32_t whence) {
    auto* file = impl->find(descriptor);
    if (!file) return kernelError(BadDescriptor);
    if (whence < 0 || whence > 2) return kernelError(Invalid);
    if (file->directory) {
        // Directory offsets are entry indexes; SEEK_SET 0 rewinds and takes a fresh listing.
        if (whence == 2) return kernelError(Invalid);
        const auto base = whence == 0 ? 0 : static_cast<std::int64_t>(file->cursor);
        if (offset < -base || offset > std::numeric_limits<std::int64_t>::max() - base) return kernelError(Invalid);
        file->cursor = static_cast<std::size_t>(base + offset);
        if (!file->cursor) file->entries.reset();
        return static_cast<std::int64_t>(file->cursor);
    }
    const auto result = ::lseek(file->file.value, static_cast<off_t>(offset), whence);
    return result < 0 ? nativeError(errno) : result;
}

std::int64_t GuestFiles::Close(std::int32_t descriptor) {
    const auto found = impl->descriptors.find(descriptor);
    if (found == impl->descriptors.end()) return kernelError(BadDescriptor);
    const int result = ::close(std::exchange(found->second.file.value, -1));
    const int error = errno;
    impl->descriptors.erase(found);
    return result < 0 ? nativeError(error) : 0;
}

std::int64_t GuestFiles::Stat(std::uint64_t path, std::uint64_t output) {
    Location location;
    if (const auto error = impl->resolve(path, location)) return error;
    struct stat status {};
    if (const auto error = impl->status(location, status)) return error;
    return impl->storeStatus(status, output);
}

std::int64_t GuestFiles::Fstat(std::int32_t descriptor, std::uint64_t output) {
    auto* file = impl->find(descriptor);
    if (!file) return kernelError(BadDescriptor);
    struct stat status {};
    if (::fstat(file->file.value, &status) < 0) return nativeError(errno);
    return impl->storeStatus(status, output);
}

std::int64_t GuestFiles::Getdents(std::int32_t descriptor, std::uint64_t buffer, std::uint64_t count) {
    return impl->directoryEntries(descriptor, buffer, count, 0);
}

std::int64_t GuestFiles::Getdirentries(std::int32_t descriptor, std::uint64_t buffer, std::uint64_t count, std::uint64_t position) {
    return impl->directoryEntries(descriptor, buffer, count, position);
}

std::int64_t GuestFiles::Fsync(std::int32_t descriptor) {
    auto* file = impl->find(descriptor);
    if (!file) return kernelError(BadDescriptor);
    return ::fsync(file->file.value) < 0 ? nativeError(errno) : 0;
}

std::int64_t GuestFiles::Ftruncate(std::int32_t descriptor, std::int64_t length) {
    auto* file = impl->find(descriptor);
    if (!file) return kernelError(BadDescriptor);
    if (length < 0 || file->directory) return kernelError(Invalid);
    return ::ftruncate(file->file.value, static_cast<off_t>(length)) < 0 ? nativeError(errno) : 0;
}

std::int64_t GuestFiles::Mkdir(std::uint64_t path, std::uint16_t) {
    return impl->mutate(path, true, kernelError(Exists), [](int parent, const char* leaf) { return ::mkdirat(parent, leaf, 0777); });
}

std::int64_t GuestFiles::Rmdir(std::uint64_t path) {
    {
        // FreeBSD refuses to remove "dir/." with EINVAL instead of removing dir.
        Location location;
        if (const auto error = impl->resolve(path, location)) return error;
        if (location.finalDot && !location.leaf.empty()) return kernelError(Invalid);
    }
    return impl->mutate(path, false, kernelError(Busy), [](int parent, const char* leaf) { return ::unlinkat(parent, leaf, AT_REMOVEDIR); });
}

std::int64_t GuestFiles::Unlink(std::uint64_t path) {
    return impl->mutate(path, false, kernelError(NotPermitted), [](int parent, const char* leaf) { return ::unlinkat(parent, leaf, 0); });
}

std::int64_t GuestFiles::Rename(std::uint64_t from, std::uint64_t to) {
    Location source, target;
    if (const auto error = impl->resolve(from, source)) return error;
    if (const auto error = impl->resolve(to, target)) return error;
    if (!source.mount->writable || !target.mount->writable) return kernelError(ReadOnly);
    if (source.mount != target.mount) return kernelError(CrossDevice);
    if (source.leaf.empty() || target.leaf.empty()) return kernelError(Busy);
    if (source.finalDot || target.finalDot) return kernelError(Invalid);
    if (const auto error = impl->requireDirectory(source)) return error;
    if (const auto error = impl->requireDirectory(target)) return error;
    return ::renameat(source.parent, source.leaf.c_str(), target.parent, target.leaf.c_str()) < 0 ? nativeError(errno) : 0;
}

std::int64_t GuestFiles::CheckReachability(std::uint64_t path) {
    Location location;
    if (const auto error = impl->resolve(path, location)) return error;
    struct stat status {};
    return impl->status(location, status);
}

}
