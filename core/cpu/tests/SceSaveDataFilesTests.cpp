// Save data mounts (sceSaveDataMount3/Umount2) bound into the guest file layer, plus the
// path and asynchronous libkernel file rows (sceKernelTruncate, sceKernelUtimes,
// sceKernelAio*), driven through real x86 GOT calls against a synthetic guest.
#include <cpu/SceElf.hpp>
#include <cpu/SceKernelImports.hpp>
#include <cpu/SceLeafSystemImports.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

constexpr std::uint64_t Efault = 0xffffffff8002000eULL, Einval = 0xffffffff80020016ULL, Erofs = 0xffffffff8002001eULL,
                        Enoent = 0xffffffff80020002ULL, Ebadf = 0xffffffff80020009ULL;
constexpr std::uint64_t Busy = 0xffffffff809f0003ULL, NotMounted = 0xffffffff809f0004ULL,
                        Exists = 0xffffffff809f0007ULL, NotFound = 0xffffffff809f0008ULL,
                        MountFull = 0xffffffff809f000cULL, Internal = 0xffffffff809f000bULL;
constexpr std::uint32_t Rdonly = 1, Rdwr = 2, Create = 4, Create2 = 32;
constexpr std::uint32_t O_WRONLY_ = 1, O_CREAT_ = 0x200;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path temporaryDirectory(const char* prefix) {
    std::string pattern = std::filesystem::temp_directory_path() / (std::string(prefix) + "-XXXXXX");
    const auto created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("Cannot create save data fixture directory");
    return created;
}

std::string contents(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

Cpu::SceImport import(const char* nid, const char* library, const char* module) {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = library;
    value.ModuleName = module;
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    return value;
}

// Code 0x1000, gate slot 0x2000, strings 0x3000, stack 0x4000, buffers 0x5000-0x8fff.
struct Session {
    std::filesystem::path resources = temporaryDirectory("anyps5-save-resources");
    std::filesystem::path titleData = temporaryDirectory("anyps5-save-title-data");
    Cpu::Machine machine;
    Cpu::SceKernelImports kernel{machine, resources, Cpu::GuestFilesOptions{titleData}};
    Cpu::SceLeafSystemImports leaf{machine};

    explicit Session(bool storage = true) {
        std::ofstream(resources / "data.bin", std::ios::binary) << "0123456789abcdef";
        machine.Map(0x1000, 4096, Permission::Read | Permission::Execute);
        for (std::uint64_t page = 0x2000; page < 0x9000; page += 0x1000) machine.Map(page, 4096, rw);
        constexpr std::array<std::uint8_t, 7> program{0xff, 0x15, 0xfa, 0x0f, 0x00, 0x00, 0x90};  // call [0x2000]
        machine.Write(0x1000, std::as_bytes(std::span(program)));
        if (storage)
            leaf.SetSaveDataStorage({titleData / "SaveData",
                [this](const std::string& mount, const std::filesystem::path& host) { return kernel.Files().BindMount(mount, host); },
                [this](const std::string& mount) { return kernel.Files().UnbindMount(mount); }});
    }
    ~Session() {
        std::error_code error;
        std::filesystem::remove_all(resources, error);
        std::filesystem::remove_all(titleData, error);
    }

    std::uint64_t gate(std::uint64_t address, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0) {
        machine.Write(0x2000, std::as_bytes(std::span(&address, 1)));
        machine.Set(Register::Rdi, a); machine.Set(Register::Rsi, b);
        machine.Set(Register::Rdx, c); machine.Set(Register::Rcx, d);
        machine.Set(Register::Rsp, 0x4ff0);
        require(machine.Run(0x1000, 0x1006, 100) == Cpu::StopReason::Address, "Gate did not return to its x86 caller");
        return machine.Get(Register::Rax);
    }
    std::uint64_t kernelCall(const char* nid, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0) {
        return gate(kernel.Resolve(import(nid, "libkernel", "libkernel")), a, b, c, d);
    }
    std::uint64_t saveCall(const char* nid, std::uint64_t a = 0, std::uint64_t b = 0) {
        const auto address = leaf.Resolve(import(nid, "libSceSaveData_native", "libSceSaveData_native"));
        require(address.has_value(), std::string("Save data row did not resolve: ") + nid);
        return gate(*address, a, b);
    }
    void text(std::uint64_t address, const std::string& value) {
        machine.Write(address, std::as_bytes(std::span(value.c_str(), value.size() + 1)));
    }
    template<class T> void put(std::uint64_t address, T value) { machine.Write(address, std::as_bytes(std::span(&value, 1))); }
    template<class T> T get(std::uint64_t address) {
        T value{};
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    std::string point(std::uint64_t result) {
        std::array<char, 17> value{};
        machine.Read(result, std::as_writable_bytes(std::span(value.data(), 16)));
        return value.data();
    }

    // SaveDataMount3 at 0x5000 naming the directory at 0x5100; SaveDataMountResult at 0x5200.
    std::uint64_t mount(const std::string& directory, std::uint32_t mode) {
        std::array<std::byte, 80> request{};
        machine.Write(0x5000, request);
        std::array<char, 32> name{};
        std::copy(directory.begin(), directory.end(), name.begin());
        machine.Write(0x5100, std::as_bytes(std::span(name)));
        put<std::uint64_t>(0x5008, 0x5100);
        put<std::uint32_t>(0x5020, mode);
        std::array<std::byte, 64> stale{};
        stale.fill(std::byte{0xcc});
        machine.Write(0x5200, stale);
        return saveCall("ZP4e7rlzOUk", 0x5000, 0x5200);
    }
    std::uint64_t unmount(const std::string& value) {
        std::array<char, 16> raw{};
        std::copy(value.begin(), value.end(), raw.begin());
        machine.Write(0x5300, std::as_bytes(std::span(raw)));
        return saveCall("uW4vfTwMQVo", 0, 0x5300);
    }
    std::uint64_t writeFile(const std::string& path, const std::string& bytes) {
        text(0x3000, path);
        const auto descriptor = kernelCall("1G3lF1Gg1k8", 0x3000, O_WRONLY_ | O_CREAT_, 0666);
        if (static_cast<std::int64_t>(descriptor) < 0) return descriptor;
        text(0x6000, bytes);
        require(kernelCall("4wSze92BhLI", descriptor, 0x6000, bytes.size()) == bytes.size(), "Guest write through a mount failed");
        require(kernelCall("UK2Tl2DWUns", descriptor) == 0, "Guest close failed");
        return 0;
    }
};

// The save mount reads what the host save directory holds.
void reopenedSave(Session& session) {
    session.text(0x3000, "/savedata0/slot.bin");
    const auto descriptor = session.kernelCall("1G3lF1Gg1k8", 0x3000, 0, 0);
    require(static_cast<std::int64_t>(descriptor) > 0, "Reopened save mount lost its files");
    require(session.kernelCall("Cg4srZ6TKbU", descriptor, 0x6000, 64) == 11, "Reopened save file has the wrong size");
    require(session.kernelCall("UK2Tl2DWUns", descriptor) == 0, "Close failed");
}

void saveDataMounts() {
    Session session;
    const auto saves = session.titleData / "SaveData";
    require(session.mount("progress", Rdwr) == NotFound, "Opening a missing save directory did not report NOT_FOUND");
    require(session.mount("progress", Create) == 0, "Creating a save directory failed");
    require(session.point(0x5200) == "/savedata0" && session.get<std::uint32_t>(0x521c) == 1 &&
            session.get<std::uint64_t>(0x5210) == 0, "Mount3 result differs from upstream (point, status, blocks)");
    require(std::filesystem::is_directory(saves / "progress"), "Mount3 did not create the host save directory");
    require(session.writeFile("/savedata0/slot.bin", "saved bytes") == 0, "Guest open under the save mount failed");
    require(contents(saves / "progress" / "slot.bin") == "saved bytes", "Save mount did not alias the host save directory");
    require(!std::filesystem::exists(session.titleData / "savedata0"), "Save mount wrote into the unbound title data mount");
    require(session.mount("progress", Rdwr) == Busy, "Mounting a mounted save directory did not report BUSY");
    require(session.mount("settings", Create2) == 0 && session.point(0x5200) == "/savedata1" &&
            session.get<std::uint32_t>(0x521c) == 1, "A second save directory did not take the next slot");
    require(session.writeFile("/savedata1/a", "1") == 0 && contents(saves / "settings" / "a") == "1", "Second save mount did not alias");

    require(session.unmount("/savedata0") == 0, "Umount2 of a mounted save failed");
    require(session.unmount("/savedata0") == NotMounted, "Repeated Umount2 did not report NOT_MOUNTED");
    require(session.unmount("/savedata7") == NotMounted, "Umount2 of an unused slot did not report NOT_MOUNTED");
    // The built-in mount returns to the title data directory; a released extra slot disappears.
    require(session.writeFile("/savedata0/after.bin", "x") == 0 && contents(session.titleData / "savedata0" / "after.bin") == "x" &&
            !std::filesystem::exists(saves / "progress" / "after.bin"), "Umount2 did not restore the default /savedata0");
    require(session.unmount("/savedata1") == 0 && session.writeFile("/savedata1/b", "1") == Enoent,
            "A released save slot still resolves");

    require(session.mount("progress", Create) == Exists, "Creating an existing save directory did not report EXISTS");
    require(session.mount("progress", Rdonly) == 0 && session.point(0x5200) == "/savedata0" &&
            session.get<std::uint32_t>(0x521c) == 0, "Reopening a save directory reported it as created");
    reopenedSave(session);
    for (int slot = 1; slot < 16; ++slot)
        require(session.mount("extra" + std::to_string(slot), Create) == 0, "Filling the save slots failed");
    require(session.mount("overflow", Create) == MountFull, "A seventeenth save mount did not report MOUNT_FULL");
}

void saveDataRejections() {
    {
        Session missing(false);
        require(missing.mount("progress", Create) == Internal, "Mount3 without a save root did not report INTERNAL");
    }
    Session session;
    for (const auto* name : {"..", "a/b", "c:d", ""}) {
        bool thrown = false;
        try { session.mount(name, Create); } catch (const std::runtime_error&) { thrown = true; }
        require(thrown, std::string("Invalid save directory name was accepted: ") + name);
    }
    bool thrown = false;
    try { session.mount("progress", 0); } catch (const std::runtime_error&) { thrown = true; }
    require(thrown, "Unknown mount mode was accepted");
    require(!std::filesystem::exists(session.titleData / "SaveData"), "A rejected mount created host directories");
}

void pathRows() {
    Session session;
    require(session.writeFile("/temp0/file", "0123456789") == 0, "Seed write failed");
    session.text(0x3000, "/temp0/file");
    require(session.kernelCall("WlyEA-sLDf0", 0x3000, 4) == 0 && contents(session.titleData / "temp0" / "file") == "0123",
            "sceKernelTruncate did not shorten the guest file");
    require(session.kernelCall("WlyEA-sLDf0", 0x3000, static_cast<std::uint64_t>(-1)) == Einval, "Negative truncate length accepted");
    session.text(0x3000, "/app0/data.bin");
    require(session.kernelCall("WlyEA-sLDf0", 0x3000, 1) == Erofs && contents(session.resources / "data.bin").size() == 16,
            "sceKernelTruncate modified the read-only resource root");
    session.text(0x3000, "/temp0/missing");
    require(session.kernelCall("WlyEA-sLDf0", 0x3000, 1) == Enoent, "Truncate of a missing file did not report ENOENT");

    session.text(0x3000, "/temp0/file");
    const std::array<std::int64_t, 4> times{1000000000, 250000, 1200000000, 500000};
    session.machine.Write(0x7000, std::as_bytes(std::span(times)));
    require(session.kernelCall("0Cq8ipKr9n0", 0x3000, 0x7000) == 0, "sceKernelUtimes failed");
    struct stat status {};
    require(::stat((session.titleData / "temp0" / "file").c_str(), &status) == 0 && status.st_mtimespec.tv_sec == 1200000000 &&
            status.st_mtimespec.tv_nsec == 500000000 && status.st_atimespec.tv_sec == 1000000000,
            "sceKernelUtimes did not set the host times");
    require(session.kernelCall("0Cq8ipKr9n0", 0x3000, 0) == 0 && ::stat((session.titleData / "temp0" / "file").c_str(), &status) == 0 &&
            status.st_mtimespec.tv_sec > 1200000000, "sceKernelUtimes with null times did not use the current time");
    session.put<std::int64_t>(0x7008, 1000000);
    require(session.kernelCall("0Cq8ipKr9n0", 0x3000, 0x7000) == Einval, "Out-of-range microseconds accepted");
    require(session.kernelCall("0Cq8ipKr9n0", 0x3000, 0xdead0000) == Efault, "Unmapped times accepted");
    session.text(0x3000, "/app0/data.bin");
    require(session.kernelCall("0Cq8ipKr9n0", 0x3000, 0) == Erofs, "sceKernelUtimes touched the read-only resource root");
}

void asyncReads() {
    Session session;
    session.text(0x3000, "/app0/data.bin");
    const auto descriptor = session.kernelCall("1G3lF1Gg1k8", 0x3000, 0, 0);
    require(static_cast<std::int64_t>(descriptor) > 0, "Resource open failed");
    // Two KernelAioRwRequest records at 0x8000 with KernelAioResult records at 0x8100/0x8110.
    const auto request = [&](std::uint64_t at, std::int64_t offset, std::uint64_t count, std::uint64_t buffer,
                             std::uint64_t result, std::int32_t fd) {
        session.put(at, offset); session.put(at + 8, count); session.put(at + 16, buffer);
        session.put(at + 24, result); session.put(at + 32, fd);
    };
    request(0x8000, 4, 4, 0x6000, 0x8100, static_cast<std::int32_t>(descriptor));
    request(0x8028, 10, 6, 0x6100, 0x8110, static_cast<std::int32_t>(descriptor));
    require(session.kernelCall("HgX7+AORI58", 0x8000, 2, 0, 0x8200) == 0, "sceKernelAioSubmitReadCommands failed");
    const auto id = session.get<std::int32_t>(0x8200);
    require(id > 0 && session.get<std::int64_t>(0x8100) == 4 && session.get<std::uint32_t>(0x8108) == 3 &&
            session.get<std::int64_t>(0x8110) == 6 && session.get<std::uint32_t>(0x8118) == 3, "Aio results differ");
    std::array<char, 7> bytes{};
    session.machine.Read(0x6000, std::as_writable_bytes(std::span(bytes.data(), 4)));
    require(std::string(bytes.data()) == "4567", "Aio read the wrong bytes");
    session.machine.Read(0x6100, std::as_writable_bytes(std::span(bytes.data(), 6)));
    require(std::string(bytes.data()) == "abcdef", "Aio second request read the wrong bytes");
    require(session.kernelCall("KOF-oJbQVvc", id, 0x8300, 0) == 0 && session.get<std::int32_t>(0x8300) == 3, "AioWaitRequest state differs");
    session.put<std::int32_t>(0x8400, id);
    require(session.kernelCall("lgK+oIWkJyA", 0x8400, 1, 0x8410, 1) == 0 && session.get<std::int32_t>(0x8410) == 3,
            "AioWaitRequests state differs");
    // A bad descriptor aborts the batch and reports EBADF in its result.
    request(0x8000, 0, 4, 0x6000, 0x8100, 77);
    require(session.kernelCall("HgX7+AORI58", 0x8000, 1, 0, 0x8200) == 0 && session.get<std::int64_t>(0x8100) == static_cast<std::int64_t>(Ebadf) &&
            session.get<std::uint32_t>(0x8108) == 4, "Aborted Aio request result differs");
    const auto aborted = session.get<std::int32_t>(0x8200);
    require(aborted != id && session.kernelCall("KOF-oJbQVvc", aborted, 0x8300, 0) == 0 && session.get<std::int32_t>(0x8300) == 4,
            "Aborted Aio batch state differs");
    require(session.kernelCall("fR521KIGgb8", 0, 0x8300) == 0 && session.get<std::int32_t>(0x8300) == 2, "Cancel of id 0 differs");
    require(session.kernelCall("fR521KIGgb8", id, 0x8300) == 0 && session.get<std::int32_t>(0x8300) == 4, "Cancel did not abort");
    session.put<std::int32_t>(0x8300, -1);
    require(session.kernelCall("5TgME6AYty4", id, 0x8300) == 0 && session.get<std::int32_t>(0x8300) == 0, "Delete did not report 0");
    require(session.kernelCall("HgX7+AORI58", 0x8000, 0, 0, 0x8200) == Einval, "Empty Aio batch accepted");
    require(session.kernelCall("HgX7+AORI58", 0, 1, 0, 0x8200) == Efault, "Null Aio batch accepted");
    require(session.kernelCall("KOF-oJbQVvc", 512, 0x8300, 0) == Einval, "Out-of-range Aio id accepted");
    require(session.kernelCall("KOF-oJbQVvc", id, 0, 0) == Efault, "Null Aio state accepted");
    require(session.kernelCall("5TgME6AYty4", 0, 0x8300) == Einval, "Delete of id 0 accepted");
}
}

int main() {
    try {
        saveDataMounts();
        saveDataRejections();
        pathRows();
        asyncReads();
        std::cout << "PASS save data mounts bound into guest files, truncate/utimes path rows, synchronous Aio reads\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
