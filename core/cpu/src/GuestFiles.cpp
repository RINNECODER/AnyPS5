#include <cpu/GuestFiles.hpp>
#include <array>
#include <bit>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Cpu {
namespace {

constexpr std::uint64_t maxTransfer = 16 * 1024 * 1024;
constexpr std::size_t maxPath = 1024;
constexpr std::size_t maxDescriptors = 1024;

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
    case ENODEV: return kernelError(19);
    case ENOTDIR: return kernelError(20);
    case EISDIR: return kernelError(21);
    case EINVAL: return kernelError(22);
    case ENFILE: return kernelError(23);
    case EMFILE: return kernelError(24);
    case EFBIG: return kernelError(27);
    case ENOSPC: return kernelError(28);
    case ESPIPE: return kernelError(29);
    case EROFS: return kernelError(30);
    case EAGAIN: return kernelError(35);
    case EOPNOTSUPP: return kernelError(45);
    case ELOOP: return kernelError(62);
    case ENAMETOOLONG: return kernelError(63);
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

}

struct GuestFiles::Impl {
    Machine& machine;
    Descriptor root;
    std::unordered_map<std::int32_t, Descriptor> descriptors;
    std::int32_t nextDescriptor = 3;

    Impl(Machine& guest, const std::filesystem::path& resourceRoot) : machine(guest),
        root(::open(resourceRoot.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) {
        if (root.value < 0) throw std::runtime_error("Cannot open guest /app0 resource root: native errno " + std::to_string(errno));
    }

    std::int64_t readPath(std::uint64_t address, std::string& path) const {
        for (std::size_t index = 0; index < maxPath; ++index) {
            if (address >= std::numeric_limits<std::uint64_t>::max() - index) return kernelError(14);
            std::array<std::byte, 1> byte;
            try {
                machine.CheckAccess(address + index, 1, Permission::Read);
                machine.Read(address + index, byte);
            } catch (const std::runtime_error&) { return kernelError(14); }
            if (byte[0] == std::byte{0}) return 0;
            path.push_back(static_cast<char>(std::to_integer<unsigned char>(byte[0])));
        }
        return kernelError(63);
    }

    std::int64_t read(std::int32_t descriptor, std::uint64_t destination, std::uint64_t count,
                      bool positional, std::int64_t offset) {
        const auto found = descriptors.find(descriptor);
        if (found == descriptors.end()) return kernelError(9);
        if (count > maxTransfer || (positional && offset < 0)) return kernelError(22);
        if (!count) return 0;
        if (count > std::numeric_limits<std::uint64_t>::max() - destination) return kernelError(14);
        try { machine.CheckAccess(destination, static_cast<std::size_t>(count), Permission::Write); }
        catch (const std::runtime_error&) { return kernelError(14); }
        std::vector<std::byte> bytes(static_cast<std::size_t>(count));
        const auto result = positional ? ::pread(found->second.value, bytes.data(), bytes.size(), static_cast<off_t>(offset)) :
                                        ::read(found->second.value, bytes.data(), bytes.size());
        if (result < 0) return nativeError(errno);
        if (result) machine.Write(destination, std::span(bytes).first(static_cast<std::size_t>(result)));
        return result;
    }
};

GuestFiles::GuestFiles(Machine& machine, const std::filesystem::path& resourceRoot) :
    impl(std::make_unique<Impl>(machine, resourceRoot)) {}
GuestFiles::~GuestFiles() = default;

std::int64_t GuestFiles::Open(std::uint64_t address, std::uint32_t flags, std::uint16_t) {
    if (flags != 0) throw std::runtime_error("Unsupported guest /app0 open flags: " + std::to_string(flags));
    std::string path;
    if (const auto error = impl->readPath(address, path)) return error;
    if (path.empty()) return kernelError(2);
    if (path == "/app0" || path == "/app0/") return kernelError(21);
    if (!path.starts_with("/app0/")) return kernelError(13);
    std::vector<std::string> components;
    for (std::size_t begin = 6; begin < path.size();) {
        const auto end = path.find('/', begin);
        const auto component = path.substr(begin, end == std::string::npos ? end : end - begin);
        if (component.empty() || component == "." || component == "..") return kernelError(13);
        components.push_back(component);
        if (end == std::string::npos) break;
        begin = end + 1;
        if (begin == path.size()) return kernelError(20);
    }
    if (impl->descriptors.size() >= maxDescriptors || impl->nextDescriptor == std::numeric_limits<std::int32_t>::max()) return kernelError(24);
    Descriptor directory;
    int parent = impl->root.value;
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        Descriptor child(::openat(parent, components[index].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (child.value < 0) return nativeError(errno);
        directory = std::move(child);
        parent = directory.value;
    }
    Descriptor file(::openat(parent, components.back().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (file.value < 0) return nativeError(errno);
    struct stat status {};
    if (::fstat(file.value, &status) < 0) return nativeError(errno);
    if (S_ISDIR(status.st_mode)) return kernelError(21);
    if (!S_ISREG(status.st_mode)) throw std::runtime_error("Unsupported guest /app0 resource type: only regular files are supported");
    const auto descriptor = impl->nextDescriptor++;
    impl->descriptors.emplace(descriptor, std::move(file));
    return descriptor;
}

std::int64_t GuestFiles::Read(std::int32_t descriptor, std::uint64_t destination, std::uint64_t count) {
    return impl->read(descriptor, destination, count, false, 0);
}

std::int64_t GuestFiles::Pread(std::int32_t descriptor, std::uint64_t destination, std::uint64_t count, std::int64_t offset) {
    return impl->read(descriptor, destination, count, true, offset);
}

std::int64_t GuestFiles::Lseek(std::int32_t descriptor, std::int64_t offset, std::int32_t whence) {
    const auto found = impl->descriptors.find(descriptor);
    if (found == impl->descriptors.end()) return kernelError(9);
    if (whence < 0 || whence > 2) return kernelError(22);
    const auto result = ::lseek(found->second.value, static_cast<off_t>(offset), whence);
    return result < 0 ? nativeError(errno) : result;
}

std::int64_t GuestFiles::Close(std::int32_t descriptor) {
    const auto found = impl->descriptors.find(descriptor);
    if (found == impl->descriptors.end()) return kernelError(9);
    const int result = ::close(std::exchange(found->second.value, -1));
    const int error = errno;
    impl->descriptors.erase(found);
    return result < 0 ? nativeError(error) : 0;
}

}
