#import <Metal/Metal.h>
#include <cpu/Cpu.hpp>
#include <cpu/ElfLoader.hpp>
#include <cpu/GuestMemoryMetal.hpp>
#include <cpu/Runtime.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceMemoryImports.hpp>
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
constexpr std::uint64_t PageBytes = 0x4000, Color = 0x200000, Alias = 0x220000;
constexpr std::uint64_t Packet = 0xb00000, Gates = Packet + 2048;
constexpr std::size_t PixelBytes = 64 * 32 * 4;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void Rejects(Function&& function, const char* diagnostic) {
    try { function(); }
    catch (const std::exception& error) {
        Require(std::string(error.what()).find(diagnostic) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing real Metal failure: ") + diagnostic);
}

struct Page {
    std::uint64_t address;
    std::unique_ptr<std::byte, decltype(&std::free)> bytes{nullptr, &std::free};
    explicit Page(std::uint64_t value) : address(value) {
        const auto alignment = sysconf(_SC_PAGESIZE);
        Require(alignment > 0 && PageBytes % alignment == 0, "Unsupported host page size");
        void* allocation = nullptr;
        Require(posix_memalign(&allocation, alignment, PageBytes) == 0, "Pinned guest allocation failed");
        bytes.reset(static_cast<std::byte*>(allocation));
        std::memset(bytes.get(), 0, PageBytes);
    }
    std::span<std::byte> Bytes() { return {bytes.get(), PageBytes}; }
};

struct DriverSession {
    AgcDriver::Metal::MetalDriver& driver;
    ~DriverSession() {
        try { driver.Shutdown(); }
        catch (...) {}
    }
};

Cpu::SceImport MemoryImport(const char* nid) {
    Cpu::SceImport result;
    result.Nid = nid;
    result.LibraryName = result.ModuleName = "libkernel";
    result.LibraryId = 7;
    result.ModuleId = 11;
    result.LibraryVersion = result.ModuleMajor = result.ModuleMinor = 1;
    return result;
}

void Pixels(std::span<const std::byte> bytes, unsigned round) {
    Require(bytes.size() == PageBytes, "Physical color allocation extent changed");
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto guard = round == 1 ? 0x7b : 0x59;
        const auto position = (i - 4096) / 4;
        const bool covered = position % 64 <= 2 * (position / 64);
        const auto pixel = round == 1 ? 255 : (!covered ? 0x41 : ((i & 3) == 0 || (i & 3) == 3 ? 255 : 0));
        const auto expected = i >= 4096 && i < 4096 + PixelBytes ? pixel : guard;
        Require(bytes[i] == std::byte(expected), "Metal output or full physical allocation guard differs at " + std::to_string(i));
    }
}

void Run(const char* fixture, id<MTLDevice> device, id<MTLLibrary> library) {
    Cpu::Machine machine;
    auto pinned = std::make_shared<std::array<Page, 7>>(std::array<Page, 7>{
        Page{0x100000}, Page{0x500000}, Page{0x600000}, Page{0x700000},
        Page{0x710000}, Page{0xa00000}, Page{Packet}});
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges;
    for (auto& page : *pinned) {
        machine.MapBorrowed(page.address, page.Bytes(), Cpu::Permission::Read | Cpu::Permission::Write);
        ranges.push_back({page.address, page.Bytes(), page.address == Packet});
    }
    Cpu::LinuxRuntime linux(machine);
    auto image = Cpu::Load(machine, fixture);
    Cpu::SetupStack(machine, image, {fixture});
    machine.Set(Cpu::Register::Rsp, image.StackPointer);
    machine.Map(0x10000000, 4096, Cpu::Permission::Read | Cpu::Permission::Execute);
    auto& driver = AgcDriver::Metal::MetalDriver::Get();
    auto mappings = Cpu::BorrowGuestMemoryForMetal({0, {}, {}}, ranges, pinned);
    std::atomic<unsigned> completed{0};
    driver.Configure((__bridge void*)device, (__bridge void*)library, mappings.Ranges,
        [&](std::uint32_t queue) {
            Require(queue == 0, "Unexpected EOP queue");
            std::uint32_t label;
            std::memcpy(&label, (*pinned)[6].bytes.get() + 512, sizeof(label));
            Require(label == 0x12345678, "EOP arrived before GPU label copyback");
            completed.fetch_add(1);
        });
    DriverSession session{driver};
    auto transaction = Cpu::MakeGuestMemoryMetalTransaction(driver, ranges, pinned);
    unsigned submitted = 0, waited = 0, registered = 0, drained = 0;
    std::uint64_t firstPhysicalId = 0, firstIdentity = 0;
    std::weak_ptr<void> retired;
    std::weak_ptr<void> failedOwner;
    bool failAfterMutation = false;
    auto memory = std::make_shared<Cpu::GuestMemoryRuntime>(machine, 8 * PageBytes,
        [&](const auto& previous, const auto& next, const auto& mutateCpu) {
            if (failAfterMutation) {
                for (const auto& owner : next.Owners)
                    if (std::find(previous.Owners.begin(), previous.Owners.end(), owner) == previous.Owners.end())
                        failedOwner = owner;
            }
            transaction(previous, next, [&] {
                mutateCpu();
                if (failAfterMutation)
                    throw std::runtime_error("injected CPU mapping callback failure after mutation");
            });
            if (submitted == 1 && waited == 0) {
                Require(completed.load() == 1, "Mapping mutation returned before accepted draw and EOP completed");
                ++drained;
            }
        });
    Cpu::SceMemoryImports imports(machine, memory);
    constexpr std::array nids{"rTXw65xmLIA", "L-Q3LEjIbgA", "cQke9UuBQOk", "MBuItvba6z8", "vSMAm3cxYTY"};
    for (std::size_t i = 0; i < nids.size(); ++i) {
        const auto gate = imports.Resolve(MemoryImport(nids[i]));
        Require(gate.has_value(), "Typed memory import did not resolve");
        machine.Write(Gates + i * 8, std::as_bytes(std::span(&*gate, 1)));
    }
    const auto checkPixels = [&](unsigned round) {
        const auto snapshot = memory->Snapshot();
        Require(snapshot.Views.size() == 2, "Physical color aliases disappeared");
        const auto& color = snapshot.Views[0];
        const auto& alias = snapshot.Views[1];
        Require(color.Address == Color && alias.Address == Alias && color.Protection == 0x32 && alias.Protection == 0x11 &&
            color.Bytes.data() == alias.Bytes.data() && color.PhysicalId == alias.PhysicalId,
            "GPU copyback is not shared with the actual read-only physical alias");
        Pixels(color.Bytes, round);
        Pixels(alias.Bytes, round);
        if (round == 1) {
            firstPhysicalId = color.PhysicalId;
            firstIdentity = color.Identity;
            Require(!snapshot.Owners.empty(), "Physical allocation has no lifetime owner");
            retired = snapshot.Owners.front();
        } else {
            Require(color.PhysicalId != firstPhysicalId && color.Identity != firstIdentity,
                "Same-VA reallocation reused stale physical or mapping identity");
            Require(retired.expired(), "Drained retired physical allocation is still retained after release");
        }
    };
    machine.AddHostCall(0x10000000, [&](Cpu::Machine& cpu) {
        const auto address = cpu.Get(Cpu::Register::Rdi);
        Require(registered < 3 && address == (registered == 0 ? 0x700000u : 0x710000u), "Guest shader registration order changed");
        cpu.CheckAccess(address, sizeof(Shader) + sizeof(ShaderUserData), Cpu::Permission::Read);
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(address));
        ++registered;
    });
    machine.AddHostCall(0x10000010, [&](Cpu::Machine& cpu) {
        Require(submitted < 2 && registered == (submitted == 0 ? 2u : 3u) &&
            cpu.Get(Cpu::Register::Rdi) == Packet && cpu.Get(Cpu::Register::Rsi) == 0,
            "Guest submission did not use its authored packet and shaders");
        cpu.CheckAccess(Packet, sizeof(::Packet), Cpu::Permission::Read);
        AgcDriver::Submit(reinterpret_cast<const ::Packet*>(Packet), 0);
        ++submitted;
    });
    machine.AddHostCall(0x10000020, [&](Cpu::Machine&) {
        Require(submitted == waited + 1, "Guest wait omitted or repeated a submission");
        AgcDriverWaitIdle_nid_postfix();
        ++waited;
        Require(completed.load() == waited, "Guest wait omitted completed EOP");
        checkPixels(waited);
    });
    Require(machine.Run(image.Entry, 0, 4000000) == Cpu::StopReason::Exit, "Actual guest ELF entry did not exit");
    Require(machine.ExitCode() == 0, "Guest rejected allocator/Metal output: exit " + std::to_string(machine.ExitCode()));
    Require(submitted == 2 && waited == 2 && registered == 3 && drained == 1,
        "Guest omitted a mapping drain, distinct draw, completion or shader update");
    checkPixels(2);
    auto last = memory->Snapshot();
    std::weak_ptr<void> finalOwner = last.Owners.front();
    last = {};
    failAfterMutation = true;
    Rejects([&] { memory->MapFlexible(0x240000, PageBytes, 0x32, 0x90); }, "injected CPU mapping callback failure after mutation");
    Require(!failedOwner.expired(), "Actual Metal driver did not retain failed candidate backing");
    machine.CheckAccess(0x240000, PageBytes, Cpu::Permission::Read);
    Rejects([&] { memory->Query(Color); }, "shutdown required");
    memory->Shutdown();
    Require(!finalOwner.expired() && !failedOwner.expired(), "CPU teardown freed previous or candidate backing before failed GPU shutdown");
    Rejects([&] { driver.Shutdown(); }, "injected CPU mapping callback failure after mutation");
    Require(finalOwner.expired() && failedOwner.expired(), "Physical backing remained live after failed GPU shutdown");
    std::cout << "PASS actual x86 typed allocation gates -> native Metal full white then half red triangles with preserved fresh background; physical read-only alias; "
        "16KiB allocation guards; same-VA fresh owner; accepted-work mutation drain; two completed EOPs; "
        "actual driver retains old and candidate leases after CPU callback failure until shutdown; guest exit=0\n";
}
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            Require(argc == 3, "Memory Metal guest test requires x86 ELF and utility metallib");
            auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "Metal 3 device unavailable");
            NSError* error = nil;
            auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]] error:&error];
            const auto diagnostic = error.localizedDescription.UTF8String;
            Require(library != nil, std::string("Utility metallib unavailable: ") + (diagnostic ? diagnostic : "unknown"));
            Run(argv[1], device, library);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << error.what() << '\n';
            return 1;
        }
    }
}
