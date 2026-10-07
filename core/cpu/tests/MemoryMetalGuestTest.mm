#import <Metal/Metal.h>
#include <cpu/Cpu.hpp>
#include <cpu/ElfLoader.hpp>
#include <cpu/GuestMemoryMetal.hpp>
#include <cpu/Runtime.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceMemoryImports.hpp>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
constexpr std::uint64_t PageBytes = 0x4000, Color = 0x200000, Alias = 0x220000;
constexpr std::uint64_t Packet = 0xb00000, Gates = Packet + 2048;
constexpr std::size_t PixelBytes = 64 * 32 * 4;
std::uint64_t FixtureClock(void*) { return 101; }
void UnexpectedFlip(void*, VideoOutConfig&, std::int64_t) {
    throw std::runtime_error("Memory composition draw fixture unexpectedly presented a VideoOut flip");
}

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

Cpu::SceImport VideoImport(const char* nid) {
    Cpu::SceImport result;
    result.Nid = nid; result.LibraryName = result.ModuleName = "libSceVideoOut";
    result.LibraryVersion = result.ModuleMajor = result.ModuleMinor = 1;
    result.LibraryId = 39; result.ModuleId = 40;
    return result;
}

void InvocationContracts() {
    // These intentionally faulty publishers exercise the public synchronous
    // CPU mutation contract. The CPU action writes real Machine memory; it is
    // never a mocked rendering result or a production-only testing hook.
    for (unsigned fault = 0; fault < 6; ++fault) {
        Cpu::Machine machine;
        machine.Map(0x40000,4096,Cpu::Permission::Read | Cpu::Permission::Write);
        Cpu::GuestMemoryMetalCompositor compositor(machine,machine.PinOwnedMappings());
        Cpu::GuestMemoryRuntime runtime(machine,PageBytes);
        auto previous = runtime.Snapshot();
        compositor.BindRuntime(previous);
        auto next = previous;
        next.Generation = 1;
        unsigned mutations = 0;
        std::function<void()> escaped;
        auto transaction = compositor.MakeTransaction([&](const auto&,auto,const auto& mutate,auto,auto) {
            if (fault == 0) escaped = mutate;
            if (fault == 1) {
                mutate();
                Rejects([&] { mutate(); },"exactly once synchronously");
            }
            if (fault == 2) {
                std::exception_ptr failure;
                std::thread foreign([&] {
                    try { Rejects([&] { mutate(); },"owner thread"); }
                    catch (...) { failure = std::current_exception(); }
                });
                foreign.join();
                if (failure) std::rethrow_exception(failure);
            }
            if (fault == 3) (void)compositor.InitialMappings();
            if (fault == 4) mutate();
            if (fault == 5) {
                machine.Unmap(0x40000,4096);
                machine.Map(0x40000,4096,Cpu::Permission::Read | Cpu::Permission::Write);
                mutate();
            }
        });
        const auto generation = compositor.Generation();
        Rejects([&] { transaction(previous,next,[&] {
            ++mutations;
            if (fault == 4) {
                machine.Unmap(0x40000,4096);
                machine.Map(0x40000,4096,Cpu::Permission::Read | Cpu::Permission::Write);
            }
            const std::byte value{0x5a};
            machine.Write(0x40000,std::span(&value,1));
        }); },fault >= 4 ? "Owned guest mapping" : (fault == 3 ? "publication reentry" : "exactly once synchronously"));
        std::byte actual{};
        machine.Read(0x40000,std::span(&actual,1));
        const auto started = fault == 1 || fault == 4;
        Require(mutations == (started ? 1u : 0u) && actual == (started ? std::byte{0x5a} : std::byte{0}) &&
            compositor.Generation() == generation,"Faulty publisher repeated/escaped CPU mutation or advanced accepted generation");
        if (escaped) Rejects([&] { escaped(); },"exactly once synchronously");
        if (started) Rejects([&] { (void)compositor.InitialMappings(); },"publication poisoned");
        else if (fault == 5) Rejects([&] { (void)compositor.InitialMappings(); },"Owned guest mapping");
        else Require(!compositor.InitialMappings().Ranges.empty(),"Pre-CPU publisher failure poisoned an unchanged publication");
    }
}

void CompositionContracts(const char* utilityLibrary) {
    constexpr std::uint64_t display = 0x500000000, ownedAlias = 0x500100000, dynamic = 0x600000000;
    constexpr auto rw = Cpu::Permission::Read | Cpu::Permission::Write;
    // QEMU admits one active CPU bridge context. Capture genuine foreign
    // scopes serially; retained scope tokens and pins outlive its translator.
    Cpu::OwnedMappingSnapshot foreignOwned;
    Cpu::GuestMemorySnapshot foreignInitial;
    {
        Cpu::Machine previousMachine;
        previousMachine.Map(display,PageBytes,rw);
        foreignOwned = previousMachine.PinOwnedMappings();
        Cpu::GuestMemoryRuntime previousRuntime(previousMachine,PageBytes);
        foreignInitial = previousRuntime.Snapshot();
    }
    Cpu::Machine machine;
    machine.Map(display,PageBytes,rw);
    auto first = machine.PinOwnedMappings();
    Require(first.Views.size() == 1 && first.Views.front().BackingIdentity == 1,
        "Collision fixture did not independently arrange first owned allocation identity one");
    machine.MapBorrowed(ownedAlias,first.Views.front().Bytes,rw,first.Views.front().Allocation);
    machine.Map(0x70000,4096,Cpu::Permission::Read);
    const auto selected = machine.PinOwnedMappings();
    constexpr std::array badWritable{display + 1}, readOnlyWritable{std::uint64_t{0x70000}};
    Rejects([&] { Cpu::GuestMemoryMetalCompositor bad(machine,selected,badWritable); },"Writable owned mapping");
    Rejects([&] { Cpu::GuestMemoryMetalCompositor bad(machine,selected,readOnlyWritable); },"Writable owned mapping");
    Rejects([&] { Cpu::GuestMemoryMetalCompositor bad(machine,foreignOwned); },"owned mapping scope");
    Cpu::GuestMemoryMetalCompositor compositor(machine,selected);
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> published;
    std::vector<std::uint64_t> generations;
    unsigned cpuMutations = 0;
    std::unique_ptr<Cpu::SceNativeGraphicsSession> session;
    auto transaction = compositor.MakeTransaction([&](const auto& ranges,auto generation,const auto& mutate,
        auto previousOwner,auto nextOwner) {
        session->MutateBorrowedRanges(ranges,generation,[&] { ++cpuMutations; mutate(); },
            std::move(previousOwner),std::move(nextOwner));
        published.assign(ranges.begin(),ranges.end());
        generations.push_back(generation);
    });
    Cpu::GuestMemoryRuntime runtime(machine,4 * PageBytes,transaction);
    const auto empty = runtime.Snapshot();
    Cpu::GuestMemoryRuntime otherRuntime(machine,PageBytes);
    Rejects([&] { compositor.BindRuntime(foreignInitial); },"runtime scope");
    compositor.BindRuntime(empty);
    Rejects([&] { compositor.BindRuntime(empty); },"already bound");
    auto initial = compositor.InitialMappings();
    const auto initialGeneration = compositor.Generation();
    const auto rangeAt = [](const auto& ranges,std::uint64_t address) -> const AgcDriver::NativeGuestMemory::BorrowedRange& {
        for (const auto& range : ranges) if (range.guestAddress == address) return range;
        throw std::runtime_error("Native composition omitted an expected mapping");
    };
    const auto ownedIdentity = rangeAt(initial.Ranges,display).identity;
    Require(ownedIdentity && rangeAt(initial.Ranges,ownedAlias).identity == ownedIdentity &&
        rangeAt(initial.Ranges,display).host.data() == rangeAt(initial.Ranges,ownedAlias).host.data() &&
        !rangeAt(initial.Ranges,display).writable,"Owned alias identity or explicit GPU-write policy changed in composition");
    AnyPS5::Host::NativeMetalSessionConfiguration configuration;
    configuration.window = {"Composed native mapping admission fixture",64,64};
    configuration.utilityMetallib = utilityLibrary;
    configuration.initialRanges = initial.Ranges;
    configuration.initialRangeOwner = initial.Owner;
    configuration.initialGeneration = compositor.Generation();
    const VideoOutCompletionCallbacks completion{nullptr,FixtureClock,FixtureClock,UnexpectedFlip};
    session = Cpu::SceNativeGraphicsSession::CreateMainThread(machine,configuration,completion);
    machine.Map(0x1000,4096,Cpu::Permission::Read | Cpu::Permission::Execute);
    machine.Map(0x4000,4096,rw);
    machine.Map(0x8000,8192,rw);
    Require(machine.PinOwnedMappings().Generation > selected.Generation,
        "Unrelated mapping generation fixture did not mutate actual Machine metadata");
    std::exception_ptr foreignFailure;
    std::thread foreign([&] {
        try { Rejects([&] { (void)compositor.InitialMappings(); },"owner thread"); }
        catch (...) { foreignFailure = std::current_exception(); }
    });
    foreign.join();
    if (foreignFailure) std::rethrow_exception(foreignFailure);
    Require(runtime.MapFlexible(dynamic,PageBytes,0x33,0x90) == dynamic && cpuMutations == 1,
        "Native compositor failed actual dynamic mapping after unrelated source generation change");
    auto current = runtime.Snapshot();
    Require(current.Views.size() == 1 && current.Views.front().Identity == 1 &&
        rangeAt(published,display).identity == ownedIdentity &&
        rangeAt(published,dynamic).identity != ownedIdentity && rangeAt(published,dynamic).identity &&
        compositor.Generation() > initialGeneration && generations.size() == 1,
        "Source-local owned/runtime identity one collided or publication failed to advance independently");
    const auto acceptedGeneration = compositor.Generation();
    auto candidate = current;
    candidate.Generation += 1;
    bool invoked = false;
    auto wrongPrevious = current;
    wrongPrevious.Views.front().Identity += 1;
    Rejects([&] { transaction(wrongPrevious,candidate,[&] { invoked = true; }); },"previous snapshot");
    candidate.Scope = otherRuntime.Snapshot().Scope;
    Rejects([&] { transaction(current,candidate,[&] { invoked = true; }); },"runtime scope");
    candidate = current; candidate.Generation += 1;
    candidate.MachineScope = foreignInitial.MachineScope;
    Rejects([&] { transaction(current,candidate,[&] { invoked = true; }); },"runtime scope");
    candidate = current; candidate.Generation += 1; candidate.Views.front().Protection = 0x22;
    Rejects([&] { transaction(current,candidate,[&] { invoked = true; }); },"GPU write-only");
    candidate = current; candidate.Generation += 1; candidate.Views.front().Address = display;
    candidate.Views.front().Protection = 0x03;
    Rejects([&] { transaction(current,candidate,[&] { invoked = true; }); },"extents overlap");
    Require(!invoked && cpuMutations == 1 && compositor.Generation() == acceptedGeneration,
        "Foreign/invalid composition reached CPU mutation or changed accepted publication");

    const auto call = [&](const char* nid,const std::array<std::uint64_t,8>& args) {
        const auto gate = session->ResolveVideoOutPublicFixture(VideoImport(nid));
        std::array<std::uint8_t,12> code{0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0};
        std::memcpy(code.data() + 2,&gate,sizeof(gate));
        machine.Write(0x1000,std::as_bytes(std::span(code)));
        constexpr std::array registers{Cpu::Register::Rdi,Cpu::Register::Rsi,Cpu::Register::Rdx,
            Cpu::Register::Rcx,Cpu::Register::R8,Cpu::Register::R9};
        for (unsigned index = 0; index < registers.size(); ++index) machine.Set(registers[index],args[index]);
        machine.Write(0x8ff0,std::as_bytes(std::span(args).last(2)));
        machine.Set(Cpu::Register::Rsp,0x8ff0);
        Require(machine.Run(0x1000,0x100c,100) == Cpu::StopReason::Address && machine.Get(Cpu::Register::Rsp) == 0x8ff0,
            "Native composition VideoOut gate failed actual x86 CALL/RET");
        return static_cast<std::int64_t>(machine.Get(Cpu::Register::Rax));
    };
    const auto handle = call("Up36PTk687E",{255,0,0,0,0,0,0,0});
    Require(handle == 1,"Native composed VideoOut output failed to open");
    call("PjS5uASwcV8",{0x4000,0x8000000000000000ULL,1,64,64,0,0,0});
    const std::array<std::uint64_t,4> row{dynamic,0,0,0};
    machine.Write(0x4100,std::as_bytes(std::span(row)));
    Require(call("rKBUtgRrtbk",{static_cast<std::uint64_t>(handle),0,0,0x4100,1,0x4000,0,0}) == 0,
        "Native dynamic allocation failed actual VideoOut buffer registration");
    const auto beforeRejection = runtime.Snapshot();
    Rejects([&] { runtime.Unmap(dynamic,PageBytes); },"registered");
    Require(cpuMutations == 1 && compositor.Generation() == acceptedGeneration &&
        runtime.Snapshot().Generation == beforeRejection.Generation && runtime.Query(dynamic).Start == dynamic,
        "Registered VideoOut extent was rejected after CPU/runtime publication had already changed");
    machine.CheckAccess(dynamic,PageBytes,Cpu::Permission::Read | Cpu::Permission::Write);
    Require(call("N5KDtkIjjJ4",{static_cast<std::uint64_t>(handle),0,0,0,0,0,0,0}) == 0 &&
        call("uquVH4-Du78",{static_cast<std::uint64_t>(handle),0,0,0,0,0,0,0}) == 0,
        "Native admission fixture could not retire its registered VideoOut extent");
    runtime.Protect(dynamic,PageBytes,0x11);
    Require(cpuMutations == 2 && generations.size() == 2 && generations.back() > generations.front() &&
        compositor.Generation() == generations.back() && rangeAt(published,display).identity == ownedIdentity,
        "Accepted publication after rejected attempt reused generation or changed owned identity");
    runtime.Shutdown();
    current = runtime.Snapshot(); candidate = current; candidate.Generation += 1;
    machine.Unmap(display,PageBytes);
    machine.Map(display,PageBytes,rw);
    const auto callsBeforeRebind = cpuMutations;
    Rejects([&] { transaction(current,candidate,[&] { invoked = true; }); },"Owned guest mapping");
    Require(!invoked && cpuMutations == callsBeforeRebind,
        "Retired/rebound owned address reached native publisher or CPU callback");
    session->ShutdownAfterCpuStoppedMainThread();
    std::cout << "PASS source-local identity collision separation; owned aliases and independent source/publication generations; "
        "foreign/stale scope, GPU-write-only/overlap/write-policy rejection; actual x86 VideoOut registered extent rejected before CPU; "
        "owned rebind rejected before publication and immediately before/after CPU action; "
        "bounded callback missing/double/escape/foreign-thread/reentry contracts\n";
}

void Run(const char* fixture, const char* utilityLibrary, id<MTLDevice> device, id<MTLLibrary> library, bool owned) {
    Cpu::Machine machine;
    std::shared_ptr<std::array<Page, 7>> pinned;
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges;
    std::unique_ptr<Cpu::GuestMemoryMetalCompositor> compositor;
    std::byte* packetBytes = nullptr;
    if (owned) {
        constexpr std::array<std::uint64_t,7> addresses{0x100000,0x500000,0x600000,0x700000,0x710000,0xa00000,Packet};
        for (const auto address : addresses)
            machine.Map(address, PageBytes, Cpu::Permission::Read | Cpu::Permission::Write);
        auto selected = machine.PinOwnedMappings();
        for (const auto& view : selected.Views)
            if (view.Region.Address == Packet) packetBytes = view.Bytes.data();
        constexpr std::array writable{Packet};
        compositor = std::make_unique<Cpu::GuestMemoryMetalCompositor>(machine, std::move(selected), writable);
    } else {
        pinned = std::make_shared<std::array<Page, 7>>(std::array<Page, 7>{
            Page{0x100000}, Page{0x500000}, Page{0x600000}, Page{0x700000},
            Page{0x710000}, Page{0xa00000}, Page{Packet}});
        for (auto& page : *pinned) {
            machine.MapBorrowed(page.address, page.Bytes(), Cpu::Permission::Read | Cpu::Permission::Write);
            ranges.push_back({page.address, page.Bytes(), page.address == Packet});
        }
        packetBytes = (*pinned)[6].bytes.get();
    }
    Require(packetBytes != nullptr, "Real pinned packet allocation is missing");
    Cpu::LinuxRuntime linux(machine);
    auto image = Cpu::Load(machine, fixture);
    Cpu::SetupStack(machine, image, {fixture});
    machine.Set(Cpu::Register::Rsp, image.StackPointer);
    machine.Map(0x10000000, 4096, Cpu::Permission::Read | Cpu::Permission::Execute);
    auto& driver = AgcDriver::Metal::MetalDriver::Get();
    std::atomic<unsigned> completed{0};
    auto eop = [&](std::uint32_t queue) {
            Require(queue == 0, "Unexpected EOP queue");
            std::uint32_t label;
            std::memcpy(&label, packetBytes + 512, sizeof(label));
            Require(label == 0x12345678, "EOP arrived before GPU label copyback");
            completed.fetch_add(1);
        };
    Cpu::GuestMemoryRuntime::Transaction transaction;
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
    std::unique_ptr<Cpu::SceNativeGraphicsSession> graphics;
    if (owned) {
        compositor->BindRuntime(memory->Snapshot());
        auto mappings = compositor->InitialMappings();
        AnyPS5::Host::NativeMetalSessionConfiguration configuration;
        configuration.window = {"Owned loader and dynamic memory composition fixture",64,32};
        configuration.utilityMetallib = utilityLibrary;
        configuration.initialRanges = mappings.Ranges;
        configuration.initialRangeOwner = mappings.Owner;
        configuration.initialGeneration = compositor->Generation();
        configuration.eopInterrupt = eop;
        const VideoOutCompletionCallbacks completion{nullptr,FixtureClock,FixtureClock,UnexpectedFlip};
        graphics = Cpu::SceNativeGraphicsSession::CreateMainThread(machine, configuration, completion);
        transaction = compositor->MakeTransaction([&](const auto& nextRanges, auto generation,
            const auto& mutate, auto previousOwner, auto nextOwner) {
            graphics->MutateBorrowedRanges(nextRanges, generation, mutate,
                std::move(previousOwner), std::move(nextOwner));
        });
    } else {
        auto mappings = Cpu::BorrowGuestMemoryForMetal({0, {}, {}}, ranges, pinned);
        driver.Configure((__bridge void*)device, (__bridge void*)library, mappings.Ranges, eop);
        transaction = Cpu::MakeGuestMemoryMetalTransaction(driver, ranges, pinned);
    }
    DriverSession session{driver};
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
    if (graphics) {
        Rejects([&] { graphics->ShutdownAfterCpuStoppedMainThread(); }, "injected CPU mapping callback failure after mutation");
        transaction = {};
        compositor.reset();
    } else {
        Rejects([&] { driver.Shutdown(); }, "injected CPU mapping callback failure after mutation");
    }
    Require(finalOwner.expired() && failedOwner.expired(), "Physical backing remained live after failed GPU shutdown");
    std::cout << (owned ? "PASS genuine Machine-owned loader shader/packet/static bytes through native graphics-session compositor; " : "PASS legacy pinned Page memory regression; ")
        << "actual x86 typed allocation gates -> native Metal full white then half red triangles with preserved fresh background; physical read-only alias; "
        "16KiB allocation guards; same-VA fresh owner; accepted-work mutation drain; two completed EOPs; "
        "actual driver retains old and candidate leases after CPU callback failure until shutdown; guest exit=0\n";
}
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            Require(argc == 3 || argc == 4, "Memory Metal guest test requires x86 ELF, utility metallib and optional owned/composition mode");
            const std::string mode = argc == 4 ? argv[3] : "legacy";
            Require(mode == "legacy" || mode == "owned" || mode == "composition", "Unknown memory composition fixture mode");
            auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "Metal 3 device unavailable");
            NSError* error = nil;
            auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]] error:&error];
            const auto diagnostic = error.localizedDescription.UTF8String;
            Require(library != nil, std::string("Utility metallib unavailable: ") + (diagnostic ? diagnostic : "unknown"));
            // The production driver admits one configured native session per
            // process. Each mode therefore owns exactly one native lifecycle.
            if (mode == "composition") {
                InvocationContracts();
                CompositionContracts(argv[2]);
            } else {
                Run(argv[1], argv[2], device, library, mode == "owned");
            }
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << error.what() << '\n';
            return 1;
        }
    }
}
