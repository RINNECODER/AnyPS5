#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace Cpu {

// Guest filesystem. Every guest path lives under a mount:
//   /app0                          read-only, the title's resource root
//   /temp0, /download0, /savedata0 writable, subdirectories of TitleDataRoot (created by the first write)
// Unknown mounts are ENOENT. Paths are walked one component at a time with O_NOFOLLOW from the
// mount's own directory descriptor, so neither ".." (rejected with EACCES) nor a host symlink can
// reach outside a mount root. Results follow the libkernel convention: a non-negative value, or a
// sign-extended 0x8002xxxx error code carrying the FreeBSD errno.
struct GuestFilesOptions {
    // Per-title host directory for the writable mounts. Empty disables them (ENOENT).
    std::filesystem::path TitleDataRoot;
};

class GuestFiles {
public:
    // The FreeBSD stat layout written by Stat and Fstat.
    static constexpr std::size_t StatSize = 120;

    // Refused with an exception: a title data root inside the resource root (the game dump), or a
    // resource root inside one of the writable mounts.
    GuestFiles(Machine& machine, const std::filesystem::path& resourceRoot, GuestFilesOptions options = {});
    static bool Overlaps(const std::filesystem::path& titleDataRoot, const std::filesystem::path& resourceRoot);
    ~GuestFiles();
    GuestFiles(const GuestFiles&) = delete;
    GuestFiles& operator=(const GuestFiles&) = delete;
    std::int64_t Open(std::uint64_t path, std::uint32_t flags, std::uint16_t mode);
    std::int64_t Read(std::int32_t descriptor, std::uint64_t destination, std::uint64_t count);
    std::int64_t Pread(std::int32_t descriptor, std::uint64_t destination, std::uint64_t count, std::int64_t offset);
    std::int64_t Write(std::int32_t descriptor, std::uint64_t source, std::uint64_t count);
    std::int64_t Pwrite(std::int32_t descriptor, std::uint64_t source, std::uint64_t count, std::int64_t offset);
    std::int64_t Lseek(std::int32_t descriptor, std::int64_t offset, std::int32_t whence);
    std::int64_t Close(std::int32_t descriptor);
    std::int64_t Stat(std::uint64_t path, std::uint64_t status);
    std::int64_t Fstat(std::int32_t descriptor, std::uint64_t status);
    // FreeBSD getdirentries records: u32 fileno, u16 reclen, u8 type, u8 namlen, NUL-terminated name,
    // 4-byte aligned. Getdirentries also stores the position before the read to *position when nonzero.
    std::int64_t Getdents(std::int32_t descriptor, std::uint64_t buffer, std::uint64_t count);
    std::int64_t Getdirentries(std::int32_t descriptor, std::uint64_t buffer, std::uint64_t count, std::uint64_t position);
    std::int64_t Fsync(std::int32_t descriptor);
    std::int64_t Ftruncate(std::int32_t descriptor, std::int64_t length);
    std::int64_t Mkdir(std::uint64_t path, std::uint16_t mode);
    std::int64_t Rmdir(std::uint64_t path);
    std::int64_t Unlink(std::uint64_t path);
    std::int64_t Rename(std::uint64_t from, std::uint64_t to);
    std::int64_t CheckReachability(std::uint64_t path);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
