#include <cpu/GuestFiles.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using Cpu::Register;
using Cpu::Permission;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;

// Signed libkernel error results: 0x80020000 | FreeBSD errno, sign-extended into RAX.
constexpr std::int64_t error(unsigned value) { return -2147352576LL + value; }
constexpr auto ENOENT_ = error(2), EBADF_ = error(9), EACCES_ = error(13), EFAULT_ = error(14),
               EEXIST_ = error(17), EXDEV_ = error(18), ENOTDIR_ = error(20), EISDIR_ = error(21),
               EINVAL_ = error(22), EROFS_ = error(30), ELOOP_ = error(62), ENAMETOOLONG_ = error(63),
               ENOTEMPTY_ = error(66);
// FreeBSD open flags.
constexpr std::uint64_t O_WRONLY_ = 1, O_RDWR_ = 2, O_NONBLOCK_ = 4, O_APPEND_ = 8, O_SHLOCK_ = 0x10,
                        O_FSYNC_ = 0x80, O_NOFOLLOW_ = 0x100, O_CREAT_ = 0x200, O_TRUNC_ = 0x400, O_EXCL_ = 0x800,
                        O_DSYNC_ = 0x1000, O_NOCTTY_ = 0x8000, O_DIRECT_ = 0x10000, O_DIRECTORY_ = 0x20000,
                        O_CLOEXEC_ = 0x100000;
constexpr std::uint64_t MiB = 1024 * 1024;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing resource service rejection: ") + expected);
}

std::filesystem::path temporaryDirectory(const char* prefix) {
    std::string pattern = std::string("/tmp/") + prefix + "-XXXXXX";
    const auto created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("Cannot create temporary directory");
    return created;
}

std::string contents(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

// Every path below the directory with its type and bytes, so a test can prove nothing changed.
std::map<std::string, std::string> snapshot(const std::filesystem::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto relative = entry.path().lexically_relative(root).string();
        if (entry.is_symlink()) result[relative] = "link:" + std::filesystem::read_symlink(entry.path()).string();
        else if (entry.is_directory()) result[relative] = "dir";
        else if (entry.is_regular_file()) result[relative] = contents(entry.path());
        else result[relative] = "other";
    }
    return result;
}

struct Resources {
    std::filesystem::path directory;
    Resources() {
        directory = temporaryDirectory("anyps5-guest-files");
        std::filesystem::create_directory(directory / "assets");
        constexpr std::array<unsigned char, 11> bytes{0x00, 0x80, 0xff, 'a', 'b', 'c', 0x13, 0x42, 0x65, 0x00, 0x7f};
        std::ofstream stream(directory / "assets" / "data.bin", std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        require(stream.good(), "Cannot populate independent resource bytes");
    }
    ~Resources() { std::error_code error; std::filesystem::remove_all(directory, error); }
};

// A separate per-title data directory, never inside the resource root.
struct TitleData {
    std::filesystem::path directory = temporaryDirectory("anyps5-guest-title-data");
    ~TitleData() { std::error_code error; std::filesystem::remove_all(directory, error); }
};

enum class Service : unsigned { Open, Read, Pread, Lseek, Close, Write, Pwrite, Stat, Fstat, Getdents,
                                Getdirentries, Fsync, Ftruncate, Mkdir, Rmdir, Unlink, Rename, CheckReachability, Count };

struct Session {
    Resources resources;
    std::optional<TitleData> data;
    Cpu::Machine machine;
    Cpu::GuestFiles files;

    explicit Session(bool writable = false) :
        data(writable ? std::optional<TitleData>(std::in_place) : std::nullopt),
        files(machine, resources.directory, Cpu::GuestFilesOptions{writable ? data->directory : std::filesystem::path{}}) {
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw);
        machine.Map(0x4000, 4096, rw);
        machine.Map(0x5000, 4096, rw);
        machine.Map(0x10000, 4096, rx);
        constexpr std::array<std::uint8_t, 1> stub{0xc3};
        for (unsigned index = 0; index < static_cast<unsigned>(Service::Count); ++index) {
            const auto gate = 0x10000 + index * 16;
            machine.Write(gate, std::as_bytes(std::span(stub)));
            machine.AddHostCall(gate, [this, index](Cpu::Machine& guest) {
                const auto first = guest.Get(Register::Rdi);
                const auto second = guest.Get(Register::Rsi);
                const auto third = guest.Get(Register::Rdx);
                const auto fourth = guest.Get(Register::Rcx);
                std::int64_t result = 0;
                const auto descriptor = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(first));
                switch (static_cast<Service>(index)) {
                case Service::Open: result = files.Open(first, static_cast<std::uint32_t>(second), static_cast<std::uint16_t>(third)); break;
                case Service::Read: result = files.Read(descriptor, second, third); break;
                case Service::Pread: result = files.Pread(descriptor, second, third, std::bit_cast<std::int64_t>(fourth)); break;
                case Service::Lseek: result = files.Lseek(descriptor, std::bit_cast<std::int64_t>(second), static_cast<std::int32_t>(third)); break;
                case Service::Close: result = files.Close(descriptor); break;
                case Service::Write: result = files.Write(descriptor, second, third); break;
                case Service::Pwrite: result = files.Pwrite(descriptor, second, third, std::bit_cast<std::int64_t>(fourth)); break;
                case Service::Stat: result = files.Stat(first, second); break;
                case Service::Fstat: result = files.Fstat(descriptor, second); break;
                case Service::Getdents: result = files.Getdents(descriptor, second, third); break;
                case Service::Getdirentries: result = files.Getdirentries(descriptor, second, third, fourth); break;
                case Service::Fsync: result = files.Fsync(descriptor); break;
                case Service::Ftruncate: result = files.Ftruncate(descriptor, std::bit_cast<std::int64_t>(second)); break;
                case Service::Mkdir: result = files.Mkdir(first, static_cast<std::uint16_t>(second)); break;
                case Service::Rmdir: result = files.Rmdir(first); break;
                case Service::Unlink: result = files.Unlink(first); break;
                case Service::Rename: result = files.Rename(first, second); break;
                case Service::CheckReachability: result = files.CheckReachability(first); break;
                case Service::Count: break;
                }
                guest.Set(Register::Rax, static_cast<std::uint64_t>(result));
            });
        }
    }

    void path(const std::string& value, std::uint64_t address = 0x3000) {
        machine.Write(address, std::as_bytes(std::span(value.c_str(), value.size() + 1)));
    }

    std::int64_t call(Service service, std::uint64_t first, std::uint64_t second = 0,
                      std::uint64_t third = 0, std::uint64_t fourth = 0) {
        const std::uint64_t gate = 0x10000 + static_cast<unsigned>(service) * 16;
        std::array<std::uint8_t, 23> program{0x48, 0xb8};
        for (unsigned index = 0; index < 8; ++index) program[2 + index] = static_cast<std::uint8_t>(gate >> (index * 8));
        constexpr std::array<std::uint8_t, 13> tail{0xff, 0xd0, 0x48, 0x89, 0x05, 0xed, 0x0f, 0x00, 0x00, 0x48, 0xff, 0xc3, 0x90};
        std::copy(tail.begin(), tail.end(), program.begin() + 10);
        machine.Write(0x1000, std::as_bytes(std::span(program)));
        machine.Set(Register::Rdi, first);
        machine.Set(Register::Rsi, second);
        machine.Set(Register::Rdx, third);
        machine.Set(Register::Rcx, fourth);
        machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rbx, 0x87654321);
        require(machine.Run(0x1000, 0x1017, 100) == Cpu::StopReason::Address, "Resource call did not return to actual x86 caller");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x87654322,
                "Resource gate corrupted guest stack or callee-saved continuation");
        std::uint64_t stored = 0;
        machine.Read(0x2000, std::as_writable_bytes(std::span(&stored, 1)));
        require(stored == machine.Get(Register::Rax), "Guest did not store actual resource return value");
        return std::bit_cast<std::int64_t>(stored);
    }

    // One-path call: the path goes to 0x3000.
    std::int64_t at(Service service, const std::string& value, std::uint64_t second = 0, std::uint64_t third = 0) {
        path(value);
        return call(service, 0x3000, second, third);
    }

    std::vector<std::uint8_t> bytes(std::uint64_t address, std::size_t count) {
        std::vector<std::uint8_t> output(count);
        machine.Read(address, std::as_writable_bytes(std::span(output)));
        return output;
    }

    template<class T> T scalar(std::uint64_t address) {
        T value{};
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }

    void put(std::uint64_t address, const std::string& value) {
        machine.Write(address, std::as_bytes(std::span(value.data(), value.size())));
    }
};

void resourceReads() {
    Session session;
    session.path("/app0/assets/data.bin");
    const auto descriptor = session.call(Service::Open, 0x3000, 0, 0777);
    require(descriptor == 3, "First guest descriptor was not allocated independently");
    constexpr std::array<std::uint8_t, 12> sentinel{0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a};
    session.machine.Write(0x5000, std::as_bytes(std::span(sentinel)));
    require(session.call(Service::Read, descriptor, 0x5000, 4) == 4, "Sequential read returned wrong byte count");
    require(session.bytes(0x5000, 5) == std::vector<std::uint8_t>{0, 0x80, 0xff, 'a', 0x5a}, "Read copied wrong file bytes or overran output");
    require(session.call(Service::Pread, descriptor, 0x5004, 3, 7) == 3, "Positional read returned wrong byte count");
    require(session.bytes(0x5004, 4) == std::vector<std::uint8_t>{0x42, 0x65, 0, 0x5a}, "Pread ignored offset or overran output");
    require(session.call(Service::Lseek, descriptor, 0, 1) == 4, "Pread changed sequential file position");
    require(session.call(Service::Read, descriptor, 0x5000, 12) == 7, "Read fabricated bytes beyond actual EOF");
    require(session.bytes(0x5000, 7) == std::vector<std::uint8_t>{'b', 'c', 0x13, 0x42, 0x65, 0, 0x7f}, "Short read returned wrong resource suffix");
    require(session.call(Service::Read, descriptor, 0x5000, 12) == 0, "EOF was not reported as zero bytes");
    require(session.call(Service::Lseek, descriptor, static_cast<std::uint64_t>(-2), 2) == 9, "SEEK_END failed signed offset");
    require(session.call(Service::Read, descriptor, 0x5000, 2) == 2 && session.bytes(0x5000, 2) == std::vector<std::uint8_t>{0, 0x7f}, "Seek did not reposition actual resource descriptor");
    require(session.call(Service::Close, descriptor) == 0, "Guest close failed");
    require(session.call(Service::Read, descriptor, 0x5000, 1) == EBADF_, "Closed descriptor did not return encoded EBADF");
    require(session.call(Service::Close, descriptor) == EBADF_, "Double close did not return encoded EBADF");
    require(session.call(Service::Open, 0x3000) == 4, "Closed guest descriptor was reused, making stale handle valid");
}

void memoryPreflight() {
    Session session;
    require(session.call(Service::Open, 0x9000) == EFAULT_, "Unmapped guest path did not return EFAULT");
    session.path("/app0/assets/data.bin");
    const auto descriptor = session.call(Service::Open, 0x3000);
    require(descriptor == 3, "Failed path read consumed guest handle");
    session.machine.Protect(0x5000, 4096, Permission::Read);
    require(session.call(Service::Read, descriptor, 0x5000, 1) == EFAULT_, "Read bypassed guest write permissions");
    require(session.call(Service::Lseek, descriptor, 0, 1) == 0, "Rejected read consumed file bytes");
    session.machine.Protect(0x5000, 4096, rw);
    constexpr std::array<std::uint8_t, 2> sentinel{0x71, 0xc2};
    session.machine.Write(0x5ffe, std::as_bytes(std::span(sentinel)));
    require(session.call(Service::Read, descriptor, 0x5ffe, 4) == EFAULT_, "Cross-page read did not preflight complete destination");
    require(session.bytes(0x5ffe, 2) == std::vector<std::uint8_t>{0x71, 0xc2}, "Rejected read partially changed guest memory");
    require(session.call(Service::Lseek, descriptor, 0, 1) == 0, "Cross-page rejection mutated file position");
    require(session.call(Service::Pread, descriptor, std::numeric_limits<std::uint64_t>::max() - 1, 4) == EFAULT_,
            "Overflowing guest buffer did not return EFAULT");
    require(session.call(Service::Read, descriptor, 0xffffffffffffffff, 0) == 0, "Zero-byte read accessed guest memory");
    require(session.call(Service::Pread, descriptor, 0x5000, 1, static_cast<std::uint64_t>(-1)) == EINVAL_,
            "Negative pread offset did not return EINVAL");
    require(session.call(Service::Read, descriptor, 0x5000, 16 * MiB + 1) == EFAULT_,
            "Large read into an unmapped tail was not preflighted as EFAULT");
    require(session.call(Service::Read, descriptor, 0x5000, std::numeric_limits<std::uint64_t>::max()) == EINVAL_,
            "Read count beyond ssize_t did not return EINVAL");
    require(session.call(Service::Lseek, descriptor, 0, 3) == EINVAL_, "Invalid whence did not return EINVAL");
    require(session.call(Service::Lseek, descriptor, static_cast<std::uint64_t>(-1), 0) == EINVAL_,
            "Negative SEEK_SET did not map native EINVAL");
    require(session.call(Service::Read, descriptor, 0x5000, 3) == 3 && session.bytes(0x5000, 3) == std::vector<std::uint8_t>{0, 0x80, 0xff},
            "Failure paths altered next real read");
}

void pathConfinement() {
    Session session;
    for (const auto* path : {"/etc/passwd", "assets/data.bin", "/app00/assets/data.bin", "/hostapp0/data.bin", "/", "",
                             "/temp0/missing", "/download0/missing", "/savedata0/missing"})
        require(session.at(Service::Open, path) == ENOENT_, "Unknown or unconfigured mount did not return ENOENT");
    for (const auto* path : {"/app0/../outside", "/app0/assets/../../outside", "/app0/assets/../assets/data.bin", "/app0/.."})
        require(session.at(Service::Open, path) == EACCES_, "Guest '..' traversal was not rejected");
    for (const auto* path : {"/app0/assets/./data.bin", "/app0//assets/data.bin", "//app0/assets/data.bin"}) {
        const auto descriptor = session.at(Service::Open, path);
        require(descriptor > 0 && session.call(Service::Read, descriptor, 0x5000, 2) == 2 &&
                session.bytes(0x5000, 2) == std::vector<std::uint8_t>{0, 0x80}, "Redundant '.' or '/' path components were not resolved inside /app0");
        require(session.call(Service::Close, descriptor) == 0, "Normalized descriptor did not close");
    }
    require(session.at(Service::Open, "/app0/missing") == ENOENT_, "Missing resource fabricated successful open");
    require(session.at(Service::Open, "/app0/assets/data.bin/child") == ENOTDIR_, "Nondirectory path component did not map ENOTDIR");
    require(session.at(Service::Open, "/app0/assets/data.bin/") == ENOTDIR_, "Trailing slash on a file did not map ENOTDIR");
    std::filesystem::create_symlink(session.resources.directory / "assets" / "data.bin", session.resources.directory / "link");
    require(session.at(Service::Open, "/app0/link") == ELOOP_, "Final symlink was followed instead of rejected");
    std::filesystem::create_directory_symlink(session.resources.directory / "assets", session.resources.directory / "linked-directory");
    const auto intermediate = session.at(Service::Open, "/app0/linked-directory/data.bin");
    require(intermediate == ELOOP_ || intermediate == ENOTDIR_, "Intermediate symlink was followed instead of rejected");
    session.path(std::string(1024, 'x'));
    require(session.call(Service::Open, 0x3000) == ENAMETOOLONG_, "Unterminated bounded guest path did not map ENAMETOOLONG");
    session.machine.Map(0x7000, 4096, rw);
    const std::string finalPath = "/app0/assets/data.bin";
    const auto pathAddress = 0x7fff - finalPath.size();
    session.path(finalPath, pathAddress);
    const auto last = session.call(Service::Open, pathAddress);
    require(last > 0, "Path terminator at final mapped byte was overread");
    require(::mkfifo((session.resources.directory / "fifo").c_str(), 0600) == 0, "Cannot create independent FIFO fixture");
    require(session.at(Service::Open, "/app0/fifo") == EACCES_, "Non-regular resource was not refused with an error code");
}

void openFlags() {
    Session session;
    const auto before = snapshot(session.resources.directory);
    // Performance and locking hints change nothing about the bytes a guest reads.
    for (const auto flags : {O_NONBLOCK_, O_SHLOCK_, O_FSYNC_, O_NOFOLLOW_, O_DSYNC_, O_NOCTTY_, O_DIRECT_, O_CLOEXEC_,
                             O_DIRECT_ | O_FSYNC_ | O_CLOEXEC_, O_CREAT_}) {
        const auto descriptor = session.at(Service::Open, "/app0/assets/data.bin", flags);
        require(descriptor > 0, "Performance open flag was not accepted on /app0");
        require(session.call(Service::Pread, descriptor, 0x5000, 3, 3) == 3 &&
                session.bytes(0x5000, 3) == std::vector<std::uint8_t>{'a', 'b', 'c'}, "Performance open flag changed file bytes");
        require(session.call(Service::Close, descriptor) == 0, "Performance-flag descriptor did not close");
    }
    for (const auto flags : {std::uint64_t{3}, std::uint64_t{0x40000}, std::uint64_t{0x80000000}})
        require(session.at(Service::Open, "/app0/assets/data.bin", flags) == EINVAL_, "Invalid open flags did not return EINVAL");
    for (const auto flags : {O_WRONLY_, O_RDWR_, O_RDWR_ | O_APPEND_, O_TRUNC_, O_WRONLY_ | O_CREAT_ | O_TRUNC_})
        require(session.at(Service::Open, "/app0/assets/data.bin", flags) == EROFS_, "Write open of /app0 did not return EROFS");
    require(session.at(Service::Open, "/app0/new.bin", O_CREAT_) == EROFS_, "Create in /app0 did not return EROFS");
    require(session.at(Service::Open, "/app0/assets/data.bin", O_CREAT_ | O_EXCL_) == EEXIST_, "O_EXCL on an existing resource did not return EEXIST");
    require(session.at(Service::Open, "/app0/assets/data.bin", O_DIRECTORY_) == ENOTDIR_, "O_DIRECTORY on a file did not return ENOTDIR");
    require(snapshot(session.resources.directory) == before, "Open flags changed the read-only resource root");
}

void largeTransfers() {
    Session session(true);
    // 16 MiB + 4099 bytes, more than one host transfer chunk, with a pattern no chunk boundary repeats.
    const std::size_t size = 16 * MiB + 4099;
    std::string pattern(size, '\0');
    for (std::size_t index = 0; index < size; ++index)
        pattern[index] = static_cast<char>((index * 131 + (index >> 12) * 7 + (index >> 24)) & 255);
    { std::ofstream stream(session.resources.directory / "assets" / "big.bin", std::ios::binary); stream << pattern; }
    constexpr std::uint64_t buffer = 0x10000000;
    session.machine.Map(buffer, 17 * MiB, rw);
    const auto descriptor = session.at(Service::Open, "/app0/assets/big.bin", O_DIRECT_);
    require(session.call(Service::Read, descriptor, buffer, size + 100) == static_cast<std::int64_t>(size),
            "Read over 16 MiB did not transfer the whole file");
    const auto read = session.bytes(buffer, size);
    require(std::equal(read.begin(), read.end(), pattern.begin(), [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); }),
            "Chunked read copied wrong bytes");
    require(session.call(Service::Pread, descriptor, buffer, 16 * MiB + 1, 3) == static_cast<std::int64_t>(16 * MiB + 1),
            "Pread over 16 MiB did not transfer the whole range");
    const auto positional = session.bytes(buffer, 16 * MiB + 1);
    require(std::equal(positional.begin(), positional.end(), pattern.begin() + 3,
                       [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); }), "Chunked pread ignored its offset");
    // Write the same >16 MiB guest buffer to a writable mount and compare the host file.
    session.machine.Write(buffer, std::as_bytes(std::span(pattern.data(), pattern.size())));
    const auto output = session.at(Service::Open, "/temp0/big.bin", O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0644);
    require(output > 0, "Cannot create a large /temp0 file");
    require(session.call(Service::Write, output, buffer, size) == static_cast<std::int64_t>(size), "Write over 16 MiB did not write every byte");
    require(session.call(Service::Pwrite, output, buffer, 16 * MiB + 1, 5) == static_cast<std::int64_t>(16 * MiB + 1),
            "Pwrite over 16 MiB did not write every byte");
    require(session.call(Service::Close, output) == 0, "Large output did not close");
    auto expected = pattern;
    expected.replace(5, 16 * MiB + 1, pattern, 0, 16 * MiB + 1);
    require(contents(session.data->directory / "temp0" / "big.bin") == expected, "Chunked write produced wrong host bytes");
}

void writableMounts() {
    Session session(true);
    const auto resources = snapshot(session.resources.directory);
    const auto root = session.data->directory;
    for (const std::string mount : {"temp0", "download0", "savedata0"}) {
        const auto directory = "/" + mount + "/dir";
        require(session.at(Service::Mkdir, directory, 0755) == 0, "Mkdir in a writable mount failed");
        require(session.at(Service::Mkdir, directory, 0755) == EEXIST_, "Repeated mkdir did not return EEXIST");
        require(std::filesystem::is_directory(root / mount / "dir"), "Mkdir did not create the host directory under the title data root");
        const auto file = directory + "/file.txt";
        const auto descriptor = session.at(Service::Open, file, O_WRONLY_ | O_CREAT_ | O_EXCL_, 0);
        require(descriptor > 0, "Create in a writable mount failed");
        session.put(0x5000, "hello " + mount);
        require(session.call(Service::Write, descriptor, 0x5000, 6 + mount.size()) == static_cast<std::int64_t>(6 + mount.size()), "Write returned wrong count");
        session.put(0x5100, "XY");
        require(session.call(Service::Pwrite, descriptor, 0x5100, 2, 20) == 2, "Pwrite returned wrong count");
        require(session.call(Service::Read, descriptor, 0x5200, 1) == EBADF_, "Read from a write-only descriptor did not return EBADF");
        require(session.call(Service::Fsync, descriptor) == 0, "Fsync of a written file failed");
        require(session.call(Service::Close, descriptor) == 0, "Writable descriptor did not close");
        std::string expected = "hello " + mount;
        expected.resize(20, '\0');
        expected += "XY";
        require(contents(root / mount / "dir" / "file.txt") == expected, "Write/Pwrite produced wrong host bytes");
        // The guest-supplied mode 0 must not make its own save unreadable.
        const auto reopened = session.at(Service::Open, file, O_RDWR_);
        require(reopened > 0 && session.call(Service::Read, reopened, 0x5200, 5) == 5 && session.bytes(0x5200, 5) == std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'},
                "A file created with guest mode 0 could not be read back");
        require(session.call(Service::Ftruncate, reopened, 3) == 0 && std::filesystem::file_size(root / mount / "dir" / "file.txt") == 3,
                "Ftruncate did not resize the host file");
        require(session.call(Service::Close, reopened) == 0, "Read-write descriptor did not close");
        require(session.at(Service::CheckReachability, file) == 0, "CheckReachability missed an existing file");
        require(session.at(Service::Unlink, file + "/") == ENOTDIR_ && std::filesystem::exists(root / mount / "dir" / "file.txt"),
                "Unlink of 'file/' removed a regular file");
        require(session.at(Service::Stat, file + "/.", 0x5000) == ENOTDIR_, "Stat of 'file/.' did not return ENOTDIR");
        require(session.at(Service::Rmdir, directory + "/.") == EINVAL_ && std::filesystem::is_directory(root / mount / "dir"),
                "Rmdir of 'dir/.' did not return EINVAL");
        require(session.at(Service::Rmdir, directory) == ENOTEMPTY_, "Rmdir of a non-empty directory did not return ENOTEMPTY");
        session.path(file, 0x3000);
        session.path(directory + "/renamed.txt", 0x3800);
        require(session.call(Service::Rename, 0x3000, 0x3800) == 0, "Rename inside a mount failed");
        require(!std::filesystem::exists(root / mount / "dir" / "file.txt") && contents(root / mount / "dir" / "renamed.txt") == "hel",
                "Rename did not move the host file");
        require(session.at(Service::CheckReachability, file) == ENOENT_, "CheckReachability found a renamed file");
        require(session.at(Service::Unlink, directory) != 0, "Unlink removed a directory");
        require(session.at(Service::Unlink, directory + "/renamed.txt") == 0, "Unlink failed");
        require(session.at(Service::Unlink, directory + "/renamed.txt") == ENOENT_, "Repeated unlink did not return ENOENT");
        require(session.at(Service::Rmdir, directory) == 0 && !std::filesystem::exists(root / mount / "dir"), "Rmdir did not remove the host directory");
        require(session.at(Service::Rmdir, "/" + mount) != 0 && std::filesystem::is_directory(root / mount), "Rmdir removed a mount root");
    }
    // Cross-mount rename is EXDEV, and nothing moves.
    const auto source = session.at(Service::Open, "/temp0/move.bin", O_WRONLY_ | O_CREAT_);
    require(source > 0 && session.call(Service::Close, source) == 0, "Cannot create rename source");
    session.path("/temp0/move.bin", 0x3000);
    session.path("/savedata0/move.bin", 0x3800);
    require(session.call(Service::Rename, 0x3000, 0x3800) == EXDEV_, "Cross-mount rename did not return EXDEV");
    require(std::filesystem::exists(root / "temp0" / "move.bin") && !std::filesystem::exists(root / "savedata0" / "move.bin"),
            "Rejected cross-mount rename moved a file");
    // Every mutation of /app0 is EROFS, and the resource root never changes.
    require(session.at(Service::Mkdir, "/app0/newdir", 0777) == EROFS_, "Mkdir in /app0 did not return EROFS");
    require(session.at(Service::Mkdir, "/app0/assets", 0777) == EEXIST_ && session.at(Service::Mkdir, "/app0", 0777) == EEXIST_,
            "Mkdir of an existing /app0 directory did not return EEXIST");
    require(session.at(Service::Unlink, "/app0/assets/data.bin") == EROFS_, "Unlink in /app0 did not return EROFS");
    require(session.at(Service::Rmdir, "/app0/assets") == EROFS_, "Rmdir in /app0 did not return EROFS");
    session.path("/app0/assets/data.bin", 0x3000);
    session.path("/temp0/stolen.bin", 0x3800);
    require(session.call(Service::Rename, 0x3000, 0x3800) == EROFS_, "Rename out of /app0 did not return EROFS");
    session.path("/temp0/move.bin", 0x3000);
    session.path("/app0/planted.bin", 0x3800);
    require(session.call(Service::Rename, 0x3000, 0x3800) == EROFS_, "Rename into /app0 did not return EROFS");
    const auto readOnly = session.at(Service::Open, "/app0/assets/data.bin");
    session.put(0x5000, "ZZ");
    require(session.call(Service::Write, readOnly, 0x5000, 2) == EBADF_, "Write to a /app0 descriptor did not return EBADF");
    require(session.call(Service::Ftruncate, readOnly, 0) != 0, "Ftruncate of a /app0 descriptor succeeded");
    require(snapshot(session.resources.directory) == resources, "Writable mount operations changed the resource root");
    // The data root is not the resource root, and no mount directory was created in the dump.
    for (const auto* mount : {"temp0", "download0", "savedata0"})
        require(!std::filesystem::exists(session.resources.directory / mount), "A writable mount was created inside the resource root");
}

void writableConfinement() {
    Session session(true);
    const auto root = session.data->directory;
    const auto outside = temporaryDirectory("anyps5-guest-outside");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{outside};
    require(session.at(Service::Mkdir, "/temp0/real", 0777) == 0, "Cannot create mount directory");
    std::filesystem::create_directory_symlink(outside, root / "temp0" / "escape");
    std::filesystem::create_symlink(outside / "target.bin", root / "temp0" / "leaf");
    for (const auto* path : {"/temp0/escape/created.bin", "/temp0/leaf"})
        require(session.at(Service::Open, path, O_WRONLY_ | O_CREAT_) < 0, "Open followed a host symlink out of the mount");
    require(session.at(Service::Mkdir, "/temp0/escape/dir", 0777) < 0, "Mkdir followed a host symlink out of the mount");
    require(session.at(Service::Stat, "/temp0/escape/x", 0x5000) < 0, "Stat followed a host symlink out of the mount");
    session.path("/temp0/real", 0x3000);
    session.path("/temp0/escape/real", 0x3800);
    require(session.call(Service::Rename, 0x3000, 0x3800) < 0, "Rename followed a host symlink out of the mount");
    require(session.at(Service::Mkdir, "/temp0/../escaped", 0777) == EACCES_, "Mkdir accepted '..'");
    require(session.at(Service::Open, "/savedata0/../temp0/x", O_WRONLY_ | O_CREAT_) == EACCES_, "Open accepted '..'");
    require(session.at(Service::Unlink, "/temp0/real/../../x") == EACCES_, "Unlink accepted '..'");
    require(std::filesystem::is_empty(outside), "A guest operation wrote outside the mount root");
    require(!std::filesystem::exists(root / "escaped") && !std::filesystem::exists(root.parent_path() / "escaped"),
            "'..' escaped the mount root");
}

// A title data root inside the game dump is refused up front.
void dataRootOutsideResources() {
    Resources resources;
    Cpu::Machine machine;
    rejects([&] { Cpu::GuestFiles files(machine, resources.directory, Cpu::GuestFilesOptions{resources.directory / "data"}); },
            "inside the resource root");
    rejects([&] { Cpu::GuestFiles files(machine, resources.directory, Cpu::GuestFilesOptions{resources.directory}); },
            "inside the resource root");
    TitleData data;
    std::filesystem::create_directories(data.directory / "savedata0" / "game");
    rejects([&] { Cpu::GuestFiles files(machine, data.directory / "savedata0" / "game", Cpu::GuestFilesOptions{data.directory}); },
            "inside a writable mount");
    require(!Cpu::GuestFiles::Overlaps(data.directory, resources.directory), "Separate title data and resource roots were reported as overlapping");
}

void statAndDirectories() {
    Session session(true);
    // FreeBSD struct stat: st_mode at 8, st_size at 72, st_blksize at 88, 120 bytes total.
    const std::vector<std::uint8_t> filler(128, 0xa5);
    session.machine.Write(0x5000, std::as_bytes(std::span(filler)));
    require(session.at(Service::Stat, "/app0/assets/data.bin", 0x5000) == 0, "Stat of a resource failed");
    require((session.scalar<std::uint16_t>(0x5008) & 0xf000) == 0x8000 && session.scalar<std::int64_t>(0x5048) == 11,
            "Stat did not report a regular 11-byte file");
    require(session.bytes(0x5078, 8) == std::vector<std::uint8_t>(8, 0xa5), "Stat wrote past the 120-byte structure");
    require(session.at(Service::Stat, "/app0/assets", 0x5000) == 0 && (session.scalar<std::uint16_t>(0x5008) & 0xf000) == 0x4000,
            "Stat did not report a directory");
    require(session.at(Service::Stat, "/app0", 0x5000) == 0 && (session.scalar<std::uint16_t>(0x5008) & 0xf000) == 0x4000,
            "Stat of a mount root did not report a directory");
    for (const auto* query : {"/temp0", "/temp0/file", "/savedata0"})
        require(session.at(Service::Stat, query, 0x5000) == ENOENT_ && session.at(Service::CheckReachability, query) == ENOENT_,
                "Query of an unwritten writable mount did not return ENOENT");
    require(session.at(Service::Open, "/download0/file") == ENOENT_, "Read-only open of an unwritten mount did not return ENOENT");
    require(std::filesystem::is_empty(session.data->directory), "A read-only query created a writable mount directory");
    require(session.at(Service::Mkdir, "/temp0/made", 0777) == 0, "Mkdir did not create the writable mount");
    require(session.at(Service::Stat, "/temp0", 0x5000) == 0 && (session.scalar<std::uint16_t>(0x5008) & 0xf000) == 0x4000,
            "Stat of a writable mount root did not report a directory");
    require(session.at(Service::Stat, "/app0/missing", 0x5000) == ENOENT_, "Stat of a missing file did not return ENOENT");
    require(session.at(Service::Stat, "/nowhere/file", 0x5000) == ENOENT_, "Stat of an unknown mount did not return ENOENT");
    require(session.at(Service::Stat, "/app0/assets/data.bin", 0x9000) == EFAULT_, "Stat into unmapped memory did not return EFAULT");
    require(session.at(Service::CheckReachability, "/app0/assets/data.bin") == 0, "CheckReachability missed a resource");
    require(session.at(Service::CheckReachability, "/app0/nope") == ENOENT_, "CheckReachability fabricated a resource");
    const auto file = session.at(Service::Open, "/app0/assets/data.bin");
    const std::vector<std::uint8_t> zeros(120, 0);
    session.machine.Write(0x5000, std::as_bytes(std::span(zeros)));
    require(session.call(Service::Fstat, file, 0x5000) == 0 && session.scalar<std::int64_t>(0x5048) == 11 &&
            (session.scalar<std::uint16_t>(0x5008) & 0xf000) == 0x8000, "Fstat did not report the open file");
    require(session.call(Service::Fstat, 999, 0x5000) == EBADF_, "Fstat of an unknown descriptor did not return EBADF");
    require(session.call(Service::Getdents, file, 0x5000, 512) == EINVAL_, "Getdents of a regular file did not return EINVAL");
    std::filesystem::create_directory(session.resources.directory / "assets" / "sub");
    { std::ofstream stream(session.resources.directory / "assets" / "second.bin"); stream << "x"; }
    const auto directory = session.at(Service::Open, "/app0/assets", O_DIRECTORY_);
    require(directory > 0, "Open of a resource directory failed");
    require(session.call(Service::Read, directory, 0x5000, 4) == EISDIR_, "Read of a directory descriptor did not return EISDIR");
    const auto parse = [&](std::uint64_t address, std::int64_t size) {
        std::map<std::string, unsigned> entries;
        const auto bytes = session.bytes(address, static_cast<std::size_t>(size));
        for (std::size_t offset = 0; offset < bytes.size();) {
            const auto length = static_cast<std::size_t>(bytes[offset + 4] | (bytes[offset + 5] << 8));
            const auto nameLength = bytes[offset + 7];
            require(length >= 8u + nameLength + 1u && length % 4 == 0 && offset + length <= bytes.size(), "Malformed dirent record");
            require(bytes[offset + 8 + nameLength] == 0, "Dirent name is not NUL-terminated");
            entries.emplace(std::string(reinterpret_cast<const char*>(bytes.data() + offset + 8), nameLength), bytes[offset + 6]);
            offset += length;
        }
        return entries;
    };
    require(session.call(Service::Getdents, directory, 0x5000, 8) == EINVAL_, "Getdents into a too-small buffer did not return EINVAL");
    const auto first = session.call(Service::Getdents, directory, 0x5000, 4096);
    require(first > 0, "Getdents returned nothing for a populated directory");
    const auto entries = parse(0x5000, first);
    require(entries == std::map<std::string, unsigned>{{".", 4}, {"..", 4}, {"data.bin", 8}, {"second.bin", 8}, {"sub", 4}},
            "Getdents did not list the directory's real entries and types");
    require(session.call(Service::Getdents, directory, 0x5000, 4096) == 0, "Getdents did not report the end of the directory");
    require(session.call(Service::Lseek, directory, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), 1) == EINVAL_,
            "Overflowing directory SEEK_CUR did not return EINVAL");
    require(session.call(Service::Lseek, directory, 0, 0) == 0, "Directory rewind failed");
    // Small buffers page through the same entries; Getdirentries reports each read's start position.
    std::map<std::string, unsigned> paged;
    std::int64_t previous = -1;
    for (int guard = 0; guard < 16; ++guard) {
        const auto count = session.call(Service::Getdirentries, directory, 0x5000, 24, 0x5800);
        require(count >= 0, "Paged getdirentries failed");
        if (!count) break;
        const auto position = session.scalar<std::int64_t>(0x5800);
        require(position > previous, "Getdirentries position did not advance");
        previous = position;
        for (const auto& entry : parse(0x5000, count)) paged.insert(entry);
    }
    require(paged == entries, "Paged getdirentries did not return the same entries");
    require(session.call(Service::Getdents, directory, 0x9000, 4096) == EFAULT_, "Getdents into unmapped memory did not return EFAULT");
    require(session.call(Service::Close, directory) == 0, "Directory descriptor did not close");
    const auto mountRoot = session.at(Service::Open, "/temp0");
    require(mountRoot > 0 && session.call(Service::Getdents, mountRoot, 0x5000, 4096) > 0, "Writable mount root could not be listed");
}
}

int main() {
    try {
        resourceReads();
        memoryPreflight();
        pathConfinement();
        openFlags();
        largeTransfers();
        writableMounts();
        writableConfinement();
        dataRootOutsideResources();
        statAndDirectories();
        std::cout << "PASS guest FD gates, real file bytes, chunked >16 MiB transfers, open flags, writable title mounts, stat/getdents and confined paths\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
