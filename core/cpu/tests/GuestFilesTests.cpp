#include <cpu/GuestFiles.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

struct Resources {
    std::filesystem::path directory;
    Resources() {
        std::array<char, 64> pattern{};
        const std::string value = "/tmp/anyps5-guest-files-XXXXXX";
        std::copy(value.begin(), value.end(), pattern.begin());
        const auto created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create temporary resource directory");
        directory = created;
        std::filesystem::create_directory(directory / "assets");
        constexpr std::array<unsigned char, 11> bytes{0x00, 0x80, 0xff, 'a', 'b', 'c', 0x13, 0x42, 0x65, 0x00, 0x7f};
        std::ofstream stream(directory / "assets" / "data.bin", std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        require(stream.good(), "Cannot populate independent resource bytes");
    }
    ~Resources() { std::error_code error; std::filesystem::remove_all(directory, error); }
};

enum class Service : unsigned { Open, Read, Pread, Lseek, Close };

struct Session {
    Resources resources;
    Cpu::Machine machine;
    Cpu::GuestFiles files{machine, resources.directory};

    Session() {
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw);
        machine.Map(0x4000, 4096, rw);
        machine.Map(0x5000, 4096, rw);
        machine.Map(0x10000, 4096, rx);
        constexpr std::array<std::uint8_t, 1> stub{0xc3};
        for (unsigned index = 0; index < 5; ++index) {
            const auto gate = 0x10000 + index * 16;
            machine.Write(gate, std::as_bytes(std::span(stub)));
            machine.AddHostCall(gate, [this, index](Cpu::Machine& guest) {
                const auto first = guest.Get(Register::Rdi);
                const auto second = guest.Get(Register::Rsi);
                const auto third = guest.Get(Register::Rdx);
                std::int64_t result = 0;
                const auto descriptor = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(first));
                switch (static_cast<Service>(index)) {
                case Service::Open: result = files.Open(first, static_cast<std::uint32_t>(second), static_cast<std::uint16_t>(third)); break;
                case Service::Read: result = files.Read(descriptor, second, third); break;
                case Service::Pread: result = files.Pread(descriptor, second, third, std::bit_cast<std::int64_t>(guest.Get(Register::Rcx))); break;
                case Service::Lseek: result = files.Lseek(descriptor, std::bit_cast<std::int64_t>(second), static_cast<std::int32_t>(third)); break;
                case Service::Close: result = files.Close(descriptor); break;
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

    std::vector<std::uint8_t> bytes(std::uint64_t address, std::size_t count) {
        std::vector<std::uint8_t> output(count);
        machine.Read(address, std::as_writable_bytes(std::span(output)));
        return output;
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
    require(session.call(Service::Read, descriptor, 0x5000, 1) == -2147352567LL, "Closed descriptor did not return encoded EBADF");
    require(session.call(Service::Close, descriptor) == -2147352567LL, "Double close did not return encoded EBADF");
    require(session.call(Service::Open, 0x3000) == 4, "Closed guest descriptor was reused, making stale handle valid");
}

void memoryPreflight() {
    Session session;
    require(session.call(Service::Open, 0x9000) == -2147352562LL, "Unmapped guest path did not return EFAULT");
    session.path("/app0/assets/data.bin");
    const auto descriptor = session.call(Service::Open, 0x3000);
    require(descriptor == 3, "Failed path read consumed guest handle");
    session.machine.Protect(0x5000, 4096, Permission::Read);
    require(session.call(Service::Read, descriptor, 0x5000, 1) == -2147352562LL, "Read bypassed guest write permissions");
    require(session.call(Service::Lseek, descriptor, 0, 1) == 0, "Rejected read consumed file bytes");
    session.machine.Protect(0x5000, 4096, rw);
    constexpr std::array<std::uint8_t, 2> sentinel{0x71, 0xc2};
    session.machine.Write(0x5ffe, std::as_bytes(std::span(sentinel)));
    require(session.call(Service::Read, descriptor, 0x5ffe, 4) == -2147352562LL, "Cross-page read did not preflight complete destination");
    require(session.bytes(0x5ffe, 2) == std::vector<std::uint8_t>{0x71, 0xc2}, "Rejected read partially changed guest memory");
    require(session.call(Service::Lseek, descriptor, 0, 1) == 0, "Cross-page rejection mutated file position");
    require(session.call(Service::Pread, descriptor, std::numeric_limits<std::uint64_t>::max() - 1, 4) == -2147352562LL,
            "Overflowing guest buffer did not return EFAULT");
    require(session.call(Service::Read, descriptor, 0xffffffffffffffff, 0) == 0, "Zero-byte read accessed guest memory");
    require(session.call(Service::Pread, descriptor, 0x5000, 1, static_cast<std::uint64_t>(-1)) == -2147352554LL,
            "Negative pread offset did not return EINVAL");
    require(session.call(Service::Read, descriptor, 0x5000, 16 * 1024 * 1024 + 1) == -2147352554LL,
            "Oversized read bypassed transfer bound");
    require(session.call(Service::Lseek, descriptor, 0, 3) == -2147352554LL, "Invalid whence did not return EINVAL");
    require(session.call(Service::Lseek, descriptor, static_cast<std::uint64_t>(-1), 0) == -2147352554LL,
            "Negative SEEK_SET did not map native EINVAL");
    require(session.call(Service::Read, descriptor, 0x5000, 3) == 3 && session.bytes(0x5000, 3) == std::vector<std::uint8_t>{0, 0x80, 0xff},
            "Failure paths altered next real read");
}

void pathConfinement() {
    Session session;
    for (const auto* path : {"/etc/passwd", "assets/data.bin", "/app00/assets/data.bin", "/app0/../outside", "/app0/assets/../../outside", "/app0/assets/./data.bin", "/app0//assets/data.bin"}) {
        session.path(path);
        require(session.call(Service::Open, 0x3000) == -2147352563LL, "Guest path escaped or bypassed strict mount components");
    }
    session.path("/app0/missing");
    require(session.call(Service::Open, 0x3000) == -2147352574LL, "Missing resource fabricated successful open");
    session.path("/app0/assets");
    require(session.call(Service::Open, 0x3000) == -2147352555LL, "Directory resource was silently treated as file");
    session.path("/app0/assets/data.bin/child");
    require(session.call(Service::Open, 0x3000) == -2147352556LL, "Nondirectory path component did not map ENOTDIR");
    std::filesystem::create_symlink(session.resources.directory / "assets" / "data.bin", session.resources.directory / "link");
    session.path("/app0/link");
    require(session.call(Service::Open, 0x3000) == -2147352514LL, "Final symlink was followed instead of rejected");
    std::filesystem::create_directory_symlink(session.resources.directory / "assets", session.resources.directory / "linked-directory");
    session.path("/app0/linked-directory/data.bin");
    const auto intermediate = session.call(Service::Open, 0x3000);
    require(intermediate == -2147352514LL || intermediate == -2147352556LL, "Intermediate symlink was followed instead of rejected");
    session.path(std::string(1024, 'x'));
    require(session.call(Service::Open, 0x3000) == -2147352513LL, "Unterminated bounded guest path did not map ENAMETOOLONG");
    session.machine.Map(0x7000, 4096, rw);
    const std::string finalPath = "/app0/assets/data.bin";
    const auto pathAddress = 0x7fff - finalPath.size();
    session.path(finalPath, pathAddress);
    require(session.call(Service::Open, pathAddress) == 3, "Path terminator at final mapped byte was overread");
    for (const auto flags : {1u, 2u, 3u, 4u, 0x200u, 0x20000u, 0x80000000u}) {
        rejects([&] { session.call(Service::Open, 0x3000, flags); }, "Unsupported guest /app0 open flags");
    }
    require(::mkfifo((session.resources.directory / "fifo").c_str(), 0600) == 0, "Cannot create independent FIFO fixture");
    session.path("/app0/fifo");
    rejects([&] { session.call(Service::Open, 0x3000); }, "only regular files");
}
}

int main() {
    try {
        resourceReads();
        memoryPreflight();
        pathConfinement();
        std::cout << "PASS guest resource FD gates, real file bytes, offsets, EOF, kernel errors, permissions and confined /app0 paths\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
