#import <Metal/Metal.h>
#include <cpu/Cpu.hpp>
#include <cpu/GuestMemoryMetal.hpp>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include <cpu/SceElf.hpp>
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Test-audit: existing fixed-selection MemoryMetalGuestTest cannot detect an
// owned child stack/TLS mapping becoming CPU-visible before native publication,
// or an owned registered extent being unmapped before VideoOut rejects it.
// This owns the staged Machine -> compositor -> real native-session boundary.
// Its x86 FS/store, real GPU compute/canary and lifecycle values are independent.
namespace {
using Cpu::Machine;
using Cpu::Permission;
using Cpu::Register;
using Compositor = Cpu::GuestMemoryMetalCompositor;
using Access = Compositor::OwnedGpuAccess;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t Page = 16384, Code = 0x1000, Stack = 0x8000;
constexpr std::uint64_t Display = 0x500000000, Commands = 0x500100000;
constexpr std::uint64_t ChildStack = 0x600000000, Dynamic = ChildStack + Page;
constexpr std::uint64_t Tls = 0x700000000, Alias = Tls + 0x100000, Failed = 0x800000000;
constexpr std::size_t ComputeOffset = 4096, ComputeThreads = 64, ComputeResults = 4;
constexpr std::size_t ComputeBytes = ComputeThreads * ComputeResults * sizeof(std::uint32_t);
// Public RDNA wave32 program already exercised by Pm4ComputeReplay. It stores
// lane id, VCC popcount, readlane(3), and EXEC selection through one real SRD.
constexpr std::array<std::uint32_t,24> ComputeCode{
    0x34020084,0xd765000a,0x000100c1,0x3604009f,0x7d880488,0xbe880f6a,0x7e160208,0xd7600009,
    0x00010700,0x7e180209,0xbe9e037e,0x7e1a0280,0x7da80488,0x7e1a0281,0xbefe031e,0xe0701000,
    0x80010a01,0xe0701004,0x80010b01,0xe0701008,0x80010c01,0xe070100c,0x80010d01,0xbf810000};

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
template<class Action> void rejects(Action&& action, const char* text) {
    try { action(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(text) != std::string::npos,error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing owned mutation rejection: ") + text);
}
template<class Action> void rejectsAny(Action&& action, const char* failure) {
    try { action(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error(failure);
}
std::uint64_t word(Machine& machine,std::uint64_t address) {
    std::uint64_t value = 0;
    machine.Read(address,std::as_writable_bytes(std::span(&value,1)));
    return value;
}
const Cpu::OwnedMappingView& at(const Cpu::OwnedMappingSnapshot& snapshot,std::uint64_t address) {
    for (const auto& view : snapshot.Views) if (view.Region.Address == address) return view;
    throw std::runtime_error("Owned mutation snapshot omitted an expected real allocation");
}
Access select(const Cpu::OwnedMappingView& view) {
    if (!(static_cast<unsigned>(view.Region.Permissions) & 1u)) return Access::CpuOnly;
    const auto address = view.Region.Address;
    if ((address >= Display && address < Display + Page) ||
        (address >= Commands && address < Commands + Page)) return Access::ReadOnly;
    if ((address >= ChildStack && address < ChildStack + Page) ||
        (address >= Tls && address < Tls + Page) ||
        (address >= Alias && address < Alias + Page) ||
        (address >= Failed && address < Failed + Page))
        return (static_cast<unsigned>(view.Region.Permissions) & 2u) ? Access::ReadWrite : Access::ReadOnly;
    return Access::CpuOnly;
}
std::uint64_t clock(void*) { return 101; }
void unexpectedFlip(void*,VideoOutConfig&,std::int64_t) {
    throw std::runtime_error("Owned mapping admission fixture unexpectedly requested a flip");
}
Cpu::SceImport videoImport(const char* nid) {
    Cpu::SceImport value;
    value.Nid = nid; value.LibraryName = value.ModuleName = "libSceVideoOut";
    value.LibraryVersion = value.ModuleMajor = value.ModuleMinor = 1;
    value.LibraryId = 39; value.ModuleId = 40;
    return value;
}

void callbackContracts() {
    // These are intentionally faulty public transaction consumers. Every
    // candidate comes from an actual Map stage, not a manufactured success.
    for (unsigned fault = 0; fault < 7; ++fault) {
        Machine machine;
        machine.Map(Code,4096,rx);
        const std::array<std::uint8_t,1> nop{0x90};
        machine.Write(Code,std::as_bytes(std::span(nop)));
        std::function<void()> escaped;
        std::weak_ptr<void> candidateOwner;
        unsigned commits = 0;
        machine.SetOwnedMappingTransaction([&](const auto& previous,const auto& next,const auto& commit) {
            machine.ValidateOwnedMappingCandidate(previous,next);
            candidateOwner = at(next,0x40000).Owner;
            if (fault == 0) { escaped = commit; return; }
            if (fault == 1) {
                commit(); ++commits;
                rejectsAny([&] { commit(); },"Double owned commit was accepted");
                return;
            }
            if (fault == 2) {
                std::exception_ptr failure;
                std::thread foreign([&] {
                    try { rejectsAny([&] { commit(); },"Foreign thread committed owned CPU bindings"); }
                    catch (...) { failure = std::current_exception(); }
                });
                foreign.join();
                if (failure) std::rethrow_exception(failure);
                return;
            }
            if (fault == 3) { machine.Map(0x80000,4096,rw); return; }
            if (fault == 4) {
                auto forged = next;
                // Preserve generation and source identity while substituting
                // another genuine allocation owner. Generation-only matching
                // cannot authorize this forged public candidate descriptor.
                for (auto& view : forged.Views)
                    if (view.Region.Address == 0x40000) view.Owner = at(previous,Code).Owner;
                machine.ValidateOwnedMappingCandidate(previous,forged);
                throw std::runtime_error("Forged owned candidate private proof was accepted");
            }
            if (fault == 5) {
                auto stale = previous;
                stale.Views.front().Region.Permissions = rw;
                machine.ValidateOwnedMappingCandidate(stale,next);
                throw std::runtime_error("Stale owned previous private proof was accepted");
            }
            commit(); ++commits;
            throw std::runtime_error("injected owned publisher failure after CPU commit");
        });
        // Check the exact private-proof errors inside the callback separately:
        // failure must originate before the deliberate post-proof sentinel.
        std::string error;
        try { machine.Map(0x40000,4096,rw); }
        catch (const std::exception& failure) { error = failure.what(); }
        require(!error.empty(),"Invalid owned transaction consumer completed its staged mapping");
        require(error != "Forged owned candidate private proof was accepted" &&
            error != "Stale owned previous private proof was accepted","Forged/stale public metadata inherited a genuine staged proof");
        if (fault == 1 || fault == 6) {
            machine.CheckAccess(0x40000,4096,rw);
            require(commits == 1 && !candidateOwner.expired(),"Post-start failure lost the actual committed allocation owner");
            rejects([&] { (void)machine.Run(Code,Code + 1,10); },"mapping transaction failed");
        } else {
            rejects([&] { machine.CheckAccess(0x40000,1,Permission::Read); },"Guest access denied");
            require(commits == 0,"Pre-CPU transaction failure committed an owned mapping");
        }
        if (escaped) {
            rejectsAny([&] { escaped(); },"Escaped owned commit changed CPU after publication returned");
            rejects([&] { machine.CheckAccess(0x40000,1,Permission::Read); },"Guest access denied");
        }
        machine.SetOwnedMappingTransaction({});
        if (fault == 1 || fault == 6) machine.Unmap(0x40000,4096);
    }
}

void nativeMutation(const char* utilityLibrary) {
    std::weak_ptr<void> terminalPrevious, terminalCandidate;
    [&] {
    Machine machine;
    machine.Map(Code,4096,rx);
    machine.Map(0x3000,4096,rx);
    machine.Map(Stack,Page,rw);
    machine.Map(Display,Page,rw);
    machine.Map(Commands,Page,rw);
    auto selected = machine.PinOwnedMappings();
    std::erase_if(selected.Views,[](const auto& view) { return select(view) == Access::CpuOnly; });
    Compositor compositor(machine,std::move(selected));
    Cpu::GuestMemoryRuntime::Transaction runtimeTransaction;
    Cpu::GuestMemoryRuntime runtime(machine,4 * Page,[&](const auto& previous,const auto& next,const auto& mutate) {
        require(static_cast<bool>(runtimeTransaction),"Unbound actual runtime transaction");
        runtimeTransaction(previous,next,mutate);
    });
    compositor.BindRuntime(runtime.Snapshot());
    auto initial = compositor.InitialMappings();
    std::mutex receiptMutex;
    std::condition_variable receiptChanged;
    std::atomic<bool> producerBlocked{false};
    bool releaseProducer = false, publisherEntered = false;
    std::atomic<unsigned> completed{0};
    std::byte* tlsBytes = nullptr;
    AnyPS5::Host::NativeMetalSessionConfiguration configuration;
    configuration.window = {"Actual owned memory publication fixture",64,64};
    configuration.utilityMetallib = utilityLibrary;
    configuration.initialRanges = initial.Ranges;
    configuration.initialRangeOwner = initial.Owner;
    configuration.initialGeneration = compositor.Generation();
    configuration.eopInterrupt = [&](std::uint32_t queue) {
        require(queue == 0 && tlsBytes != nullptr,"Actual owned EOP had wrong queue or backing");
        // RELEASE_MEM itself performs a host immediate store. Its distinct
        // label is ordering evidence; only the preceding Metal dispatch can
        // produce these 256 independently expected output words.
        for (std::uint32_t tid = 0; tid < ComputeThreads; ++tid) {
            const auto lane = tid % 32u;
            const std::array<std::uint32_t,4> expected{lane,8u,tid - lane + 3u,lane < 8u ? 1u : 0u};
            for (std::size_t column = 0; column < expected.size(); ++column) {
                std::uint32_t actual = 0;
                std::memcpy(&actual,tlsBytes + ComputeOffset + (tid * ComputeResults + column) * sizeof(actual),sizeof(actual));
                require(actual == expected[column],"Actual Metal dispatch output differs from independent lane/count/readlane/EXEC golden");
            }
        }
        std::uint32_t label = 0;
        std::memcpy(&label,tlsBytes + 256,sizeof(label));
        require(label == 0xff934317,"Immediate EOP label differs after actual Metal dispatch output copyback");
        std::unique_lock lock(receiptMutex);
        producerBlocked = true; receiptChanged.notify_all();
        require(receiptChanged.wait_for(lock,std::chrono::seconds(5),[&] { return releaseProducer; }),
            "Bounded actual pending-EOP completion barrier timed out");
        ++completed;
    };
    const VideoOutCompletionCallbacks completion{nullptr,clock,clock,unexpectedFlip};
    auto session = Cpu::SceNativeGraphicsSession::CreateMainThread(machine,configuration,completion);
    configuration.initialRangeOwner.reset(); configuration.initialRanges = {}; initial = {};
    std::vector<std::uint64_t> publicationGenerations;
    unsigned actualCpuCommits = 0;
    auto publisher = [&](const auto& ranges,auto generation,const auto& commit,auto oldOwner,auto newOwner) {
        { std::lock_guard lock(receiptMutex); publisherEntered = true; receiptChanged.notify_all(); }
        session->MutateBorrowedRanges(ranges,generation,[&] {
            if (producerBlocked) require(completed == 1,"CPU owned mutation ran before accepted native GPU/EOP drain");
            ++actualCpuCommits;
            commit();
        },std::move(oldOwner),std::move(newOwner));
        publicationGenerations.push_back(generation);
    };
    runtimeTransaction = compositor.MakeTransaction(publisher);
    auto ownedTransaction = compositor.MakeOwnedTransaction(publisher,select);
    bool injected = false;
    std::weak_ptr<void> failedOwner;
    machine.SetOwnedMappingTransaction([&](const auto& previous,const auto& next,const auto& commit) {
        if (injected) failedOwner = at(next,Failed).Owner;
        ownedTransaction(previous,next,[&] {
            commit();
            if (injected) throw std::runtime_error("injected native owned publication failure after CPU commit");
        });
    });

    constexpr std::uint64_t gate = 0x3000;
    machine.AddHostCall(gate,[&](Machine& running) {
        running.Map(ChildStack,Page,rw);
        running.Map(Tls,Page,rw);
        std::array<std::byte,Page> guard;
        guard.fill(std::byte{0xa5}); running.Write(Tls,guard);
        guard.fill(std::byte{0xc3}); running.Write(ChildStack,guard);
        const std::uint64_t input = 37;
        running.Write(Tls,std::as_bytes(std::span(&input,1)));
        running.Set(Register::FsBase,Tls);
        running.Set(Register::Rdi,Tls); running.Set(Register::Rsi,ChildStack);
        const std::array readable{AgcDriver::Metal::ReadableGuestRange{Tls,Page},
            AgcDriver::Metal::ReadableGuestRange{ChildStack,Page}};
        session->Driver().WithValidatedReadableRanges(readable,[&] {
            const auto bytes = AgcDriver::NativeGuestMemory::ContiguousBorrowedRange(Tls,Page);
            require(bytes[0] == std::byte{37} && bytes[4096] == std::byte{0xa5},
                "New actual TLS was not natively visible before the translated caller resumed");
        });
    });
    constexpr std::array<std::uint8_t,33> caller{
        0x48,0xb8,0,0x30,0,0,0,0,0,0,0xff,0xd0,
        0x64,0x48,0x8b,0x04,0x25,0,0,0,0,
        0x48,0x83,0xc0,0x04,0x48,0x89,0x47,0x08,0x48,0x89,0x46,0x18};
    machine.Write(Code,std::as_bytes(std::span(caller)));
    machine.Set(Register::Rsp,Stack + Page - 16);
    require(machine.Run(Code,Code + caller.size(),100) == Cpu::StopReason::Address &&
        machine.Get(Register::Rax) == 41 && word(machine,Tls + 8) == 41 && word(machine,ChildStack + 24) == 41,
        "Actual translated FS/TLS and child-shaped stack arithmetic lost live owned publication");
    auto pinned = machine.PinOwnedMappings();
    auto tls = at(pinned,Tls);
    tlsBytes = tls.Bytes.data();
    const auto tlsIdentity = tls.BackingIdentity;
    std::weak_ptr<void> retired = tls.Owner;
    machine.MapBorrowed(Alias,tls.Bytes,rw,tls.Allocation);
    require(at(machine.PinOwnedMappings(),Alias).BackingIdentity == tlsIdentity,
        "Live owned alias changed its actual allocation identity");
    machine.Protect(Tls,Page,Permission::Read);
    machine.ProtectFragment(Alias + 32,8,Permission::Read);
    rejects([&] { machine.CheckAccess(Alias + 32,8,Permission::Write); },"Guest access denied");
    machine.CheckAccess(Alias + 24,8,Permission::Write);
    machine.Protect(Alias,Page,rw);
    require(runtime.MapFlexible(Dynamic,Page,0x33,0x90) == Dynamic,"Actual shared runtime publication failed");
    const auto beforeMixed = compositor.Generation();
    rejectsAny([&] { machine.Protect(ChildStack,2 * Page,Permission::Read); },"Mixed owned/runtime span bypassed separate atomic publishers");
    machine.CheckAccess(ChildStack,Page,rw); machine.CheckAccess(Dynamic,Page,rw);
    require(compositor.Generation() == beforeMixed && runtime.Query(Dynamic).Protection == 0x33,
        "Rejected mixed span changed owned or runtime publication");

    // Capture real native-readable spans and compare bytes, not a shadow copy.
    // Restoring permissions preserves the structural fragments. Each native
    // contiguous read must use its actual extent, with the same allocation.
    const std::array reads{AgcDriver::Metal::ReadableGuestRange{Alias,32},
        AgcDriver::Metal::ReadableGuestRange{Alias + 32,8},
        AgcDriver::Metal::ReadableGuestRange{Alias + 40,Page - 40},
        AgcDriver::Metal::ReadableGuestRange{ChildStack,Page}};
    session->Driver().WithValidatedReadableRanges(reads,[&] {
        const auto bytes = AgcDriver::NativeGuestMemory::ContiguousBorrowedRange(Alias,32);
        require(AgcDriver::NativeGuestMemory::ContiguousBorrowedRange(Alias + 32,8).data() == tlsBytes + 32 &&
            AgcDriver::NativeGuestMemory::ContiguousBorrowedRange(Alias + 40,Page - 40).data() == tlsBytes + 40,
            "Native owned fragments changed allocation offsets");
        std::uint64_t value = 0; std::memcpy(&value,bytes.data() + 8,8);
        require(value == 41 && bytes.data() == tlsBytes,"Native owned alias copied memory or lost actual guest TLS result");
        const auto stack = AgcDriver::NativeGuestMemory::ContiguousBorrowedRange(ChildStack,Page);
        std::memcpy(&value,stack.data() + 24,8);
        require(value == 41,"Native child-shaped stack lost actual translated store");
        for (std::size_t i = 0; i < stack.size(); ++i)
            if (i < 24 || i >= 32) require(stack[i] == std::byte{0xc3},"Native child stack guard overwritten");
    });
    // Shader/code/header live in the newly published child-owned stack. The
    // real dispatch writes the newly published TLS allocation through Alias.
    constexpr std::uint64_t shaderCode = ChildStack + 4096, shaderHeader = ChildStack + 8192;
    machine.Write(shaderCode,std::as_bytes(std::span(ComputeCode)));
    std::array<std::byte,sizeof(Shader) + sizeof(ShaderUserData)> headerBytes{};
    Shader header{};
    header.file_header = 0x34333231; header.version = 0x18;
    header.code = reinterpret_cast<const volatile void*>(shaderCode);
    header.user_data = reinterpret_cast<ShaderUserData*>(shaderHeader + sizeof(Shader));
    header.header_size = headerBytes.size(); header.shader_size = sizeof(ComputeCode);
    std::memcpy(headerBytes.data(),&header,sizeof(header));
    machine.Write(shaderHeader,headerBytes);
    session->Driver().RegisterShader(reinterpret_cast<const Shader*>(shaderHeader));
    std::array<std::byte,Page> preparedStack{};
    machine.Read(ChildStack,preparedStack);
    std::vector<std::uint32_t> dispatch;
    const auto registers = [&](std::uint32_t first,std::span<const std::uint32_t> values) {
        dispatch.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16));
        dispatch.push_back(first); dispatch.insert(dispatch.end(),values.begin(),values.end());
    };
    const std::array<std::uint32_t,3> threads{ComputeThreads,1,1};
    const std::array<std::uint32_t,2> program{static_cast<std::uint32_t>(shaderCode >> 8),
        static_cast<std::uint32_t>(shaderCode >> 40)};
    const std::array<std::uint32_t,1> resources{16};
    const auto outputAddress = Alias + ComputeOffset;
    const std::array<std::uint32_t,8> users{0,0,0,0,static_cast<std::uint32_t>(outputAddress),
        static_cast<std::uint32_t>((outputAddress >> 32) & 0xffffu) | (4u << 16),
        ComputeThreads * ComputeResults,0x01016fac};
    registers(0x207,threads); registers(0x20c,program); registers(0x213,resources); registers(0x240,users);
    const std::array<std::uint32_t,5> direct{0xc0031500,1,1,1,0x8041};
    dispatch.insert(dispatch.end(),direct.begin(),direct.end());
    const std::array<std::uint32_t,8> eop{0xc0064900,0,(1u<<29)|(1u<<24),
        static_cast<std::uint32_t>(Alias + 256),static_cast<std::uint32_t>((Alias + 256) >> 32),0xff934317,0,0};
    dispatch.insert(dispatch.end(),eop.begin(),eop.end());
    machine.Write(Commands,std::as_bytes(std::span(dispatch)));
    session->Driver().SubmitCommandBuffer(Commands,dispatch.size(),0,0);
    {
        std::unique_lock lock(receiptMutex);
        require(receiptChanged.wait_for(lock,std::chrono::seconds(5),[&] { return producerBlocked.load(); }),
            "Actual native EOP did not reach bounded pending completion barrier");
        publisherEntered = false;
    }
    std::exception_ptr releaseFailure;
    std::jthread releaser([&] {
        try {
            std::unique_lock lock(receiptMutex);
            const bool entered = receiptChanged.wait_for(lock,std::chrono::seconds(5),[&] { return publisherEntered; });
            releaseProducer = true; receiptChanged.notify_all();
            require(entered,"Owned native publisher did not enter while actual accepted EOP was pending");
        } catch (...) { releaseFailure = std::current_exception(); }
    });
    machine.Unmap(Tls,Page);
    releaser.join();
    if (releaseFailure) std::rethrow_exception(releaseFailure);
    require(completed == 1 && !retired.expired(),"Pending native work failed drain or freed an owned alias prematurely");
    for (std::size_t i = 0; i < tls.Bytes.size(); ++i) {
        if (i < 16 || (i >= 256 && i < 260) || (i >= ComputeOffset && i < ComputeOffset + ComputeBytes)) continue;
        require(tls.Bytes[i] == std::byte{0xa5},"Actual owned Metal dispatch damaged TLS allocation guard");
    }
    std::array<std::byte,Page> completedStack{};
    machine.Read(ChildStack,completedStack);
    require(completedStack == preparedStack,"Actual Metal dispatch changed child-owned shader/header or allocation guards");
    tls = {}; pinned = {};
    machine.Unmap(Alias,Page);
    require(retired.expired(),"Accepted native drain/last alias removal retained retired owned storage");
    machine.Map(Tls,Page,rw);
    require(at(machine.PinOwnedMappings(),Tls).BackingIdentity != tlsIdentity && word(machine,Tls + 8) == 0,
        "Same-address owned reallocation reused stale identity or dispatch/guest bytes");

    const auto call = [&](const char* nid,const std::array<std::uint64_t,8>& args) {
        const auto address = session->ResolveVideoOutPublicFixture(videoImport(nid));
        std::array<std::uint8_t,12> code{0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0};
        std::memcpy(code.data() + 2,&address,8); machine.Write(Code,std::as_bytes(std::span(code)));
        constexpr std::array regs{Register::Rdi,Register::Rsi,Register::Rdx,Register::Rcx,Register::R8,Register::R9};
        for (std::size_t i = 0; i < regs.size(); ++i) machine.Set(regs[i],args[i]);
        machine.Write(Stack + Page - 16,std::as_bytes(std::span(args).last(2)));
        machine.Set(Register::Rsp,Stack + Page - 16);
        require(machine.Run(Code,Code + 12,100) == Cpu::StopReason::Address,"Actual owned VideoOut x86 CALL did not return");
        return machine.Get(Register::Rax);
    };
    const auto handle = call("Up36PTk687E",{255,0,0,0,0,0,0,0});
    require(handle == 1,"Actual owned display failed native VideoOut open");
    call("PjS5uASwcV8",{Stack,0x8000000000000000ULL,1,64,64,0,0,0});
    const std::array<std::uint64_t,4> buffers{Display,0,0,0};
    machine.Write(Stack + 512,std::as_bytes(std::span(buffers)));
    require(call("rKBUtgRrtbk",{handle,0,0,Stack + 512,1,Stack,0,0}) == 0,"Owned display registration failed");
    auto ownedBefore = machine.PinOwnedMappings();
    terminalPrevious = at(ownedBefore,Display).Owner;
    const auto beforeCpu = actualCpuCommits;
    const auto beforeGeneration = compositor.Generation();
    rejects([&] { machine.Unmap(Display,Page); },"registered");
    require(actualCpuCommits == beforeCpu && compositor.Generation() == beforeGeneration &&
        machine.PinOwnedMappings().Generation == ownedBefore.Generation && word(machine,Display) == 0,
        "VideoOut owned extent rejected after actual CPU binding/bytes changed");
    machine.CheckAccess(Display,Page,rw);
    require(call("N5KDtkIjjJ4",{handle,0,0,0,0,0,0,0}) == 0 && call("uquVH4-Du78",{handle,0,0,0,0,0,0,0}) == 0,
        "Actual registered owned output did not retire cleanly");
    runtime.Shutdown(); runtimeTransaction = {};
    ownedBefore = {};
    require(publicationGenerations.size() >= 10 && std::adjacent_find(publicationGenerations.begin(),publicationGenerations.end(),
        [](auto a,auto b) { return a >= b; }) == publicationGenerations.end(),
        "Owned and dynamic transactions reused shared native publication generations");
    injected = true;
    rejects([&] { machine.Map(Failed,Page,rw); },"injected native owned publication failure after CPU commit");
    terminalCandidate = failedOwner;
    require(!failedOwner.expired(),"Post-CPU native failure released actual new owned allocation");
    rejects([&] { (void)machine.Run(Code,Code + 1,10); },"mapping transaction failed");
    machine.CheckAccess(Failed,Page,rw);
    rejects([&] { session->ShutdownAfterCpuStoppedMainThread(); },"injected native owned publication failure after CPU commit");
    machine.SetOwnedMappingTransaction({});
    for (const auto address : {Tls,ChildStack,Failed,Display,Commands}) machine.Unmap(address,Page);
    require(!failedOwner.expired() && !terminalPrevious.expired(),
        "Failed compositor previous/candidate owner retired before explicit terminal lease retirement");
    }();
    require(terminalPrevious.expired() && terminalCandidate.expired(),
        "Actual previous/candidate allocation owners leaked after complete graphics/compositor/Machine retirement");
    std::cout << "PASS genuine staged owned Map/alias/Protect/fragment/Unmap through real native session; "
        "actual x86 FS and child-shaped stack byte oracles; real Metal wave32 dispatch goldens/canaries and pending EOP completion drain; "
        "shared runtime publication generations; registered owned extent rejects before CPU; fresh rebind identity; "
        "forged/stale/escaped/double/foreign/reentrant commits fail closed and post-start failure stops guest\n";
}
}

int main(int argc,const char* argv[]) {
    @autoreleasepool {
        try {
            require(argc == 2,"Owned native mutation test requires existing utility metallib");
            callbackContracts();
            nativeMutation(argv[1]);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << error.what() << '\n';
            return 1;
        }
    }
}
