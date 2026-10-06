#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace Cpu {

class GuestFiles {
public:
    GuestFiles(Machine& machine, const std::filesystem::path& resourceRoot);
    ~GuestFiles();
    GuestFiles(const GuestFiles&) = delete;
    GuestFiles& operator=(const GuestFiles&) = delete;
    std::int64_t Open(std::uint64_t path, std::uint32_t flags, std::uint16_t mode);
    std::int64_t Read(std::int32_t descriptor, std::uint64_t destination, std::uint64_t count);
    std::int64_t Pread(std::int32_t descriptor, std::uint64_t destination, std::uint64_t count, std::int64_t offset);
    std::int64_t Lseek(std::int32_t descriptor, std::int64_t offset, std::int32_t whence);
    std::int64_t Close(std::int32_t descriptor);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
