#include <cpu/SceMemoryImports.hpp>
#include <cpu/GuestMemoryRuntime.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t page = 0x4000;
constexpr std::uint64_t capacity = 8 * page;
constexpr std::uint64_t fault = 0xffffffff8002000eULL;
constexpr std::uint64_t invalid = 0xffffffff80020016ULL;
constexpr std::uint64_t exists = 0xffffffff80020011ULL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void rejects(Function&& function, const char* diagnostic) {
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(diagnostic) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing memory gate rejection: ") + diagnostic);
}
Cpu::SceImport import(const char* nid) {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = value.ModuleName = "libkernel";
    value.LibraryId = 7;
    value.ModuleId = 11;
    value.LibraryVersion = value.ModuleMajor = value.ModuleMinor = 1;
    return value;
}
struct Session {
    Cpu::Machine machine;
    std::shared_ptr<Cpu::GuestMemoryRuntime> memory;
    std::unique_ptr<Cpu::SceMemoryImports> imports;
    unsigned transactions = 0;

    explicit Session(std::uint64_t directCapacity = capacity) {
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 8192, rw);
        machine.Map(0x4000, 4096, rw);
        machine.Map(0x5000, 4096, rw);
        constexpr std::array<std::uint8_t,17> caller{
            0xff,0x15,0xfa,0x0f,0,0, 0x48,0x89,0x05,0x03,0x10,0,0, 0x48,0xff,0xc3,0x90};
        constexpr std::array<std::uint8_t,3> load{0x48,0x8b,0x07};
        constexpr std::array<std::uint8_t,3> store{0x48,0x89,0x07};
        machine.Write(0x1000, std::as_bytes(std::span(caller)));
        machine.Write(0x1020, std::as_bytes(std::span(load)));
        machine.Write(0x1030, std::as_bytes(std::span(store)));
        memory = std::make_shared<Cpu::GuestMemoryRuntime>(machine, directCapacity,
            [this](const Cpu::GuestMemorySnapshot& previous, const Cpu::GuestMemorySnapshot& next,
                   const std::function<void()>& cpuMutation) {
                require(next.Generation > previous.Generation, "Memory gate did not advance its synchronous transaction");
                cpuMutation();
                ++transactions;
            });
        imports = std::make_unique<Cpu::SceMemoryImports>(machine, memory);
    }
    template<class T> T read(std::uint64_t address) {
        T value{};
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    template<class T> void write(std::uint64_t address, const T& value) {
        machine.Write(address, std::as_bytes(std::span(&value, 1)));
    }
    std::uint64_t callGate(std::uint64_t gate, std::array<std::uint64_t,6> arguments = {}) {
        write(0x2000, gate);
        constexpr std::array registers{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
        for (std::size_t index = 0; index < arguments.size(); ++index) machine.Set(registers[index], arguments[index]);
        machine.Set(Register::R10, 0xdead0000);
        machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rbx, 0x123456789abcdeff);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address,
                "Memory gate failed to return to the actual x86 SysV caller");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x123456789abcdf00,
                "Memory gate corrupted CALL/RET or the callee-saved continuation");
        const auto result = read<std::uint64_t>(0x2010);
        require(result == machine.Get(Register::Rax), "Actual guest failed to store the memory gate result");
        return result;
    }
    std::uint64_t call(const char* nid, std::array<std::uint64_t,6> arguments = {}) {
        const auto gate = imports->Resolve(import(nid));
        require(gate.has_value(), "Audited memory NID did not resolve");
        return callGate(*gate, arguments);
    }
    std::uint64_t load(std::uint64_t address) {
        machine.Set(Register::Rdi, address);
        require(machine.Run(0x1020, 0x1023, 10) == Cpu::StopReason::Address, "Actual x86 memory load did not finish");
        return machine.Get(Register::Rax);
    }
    void store(std::uint64_t address, std::uint64_t value) {
        machine.Set(Register::Rdi, address);
        machine.Set(Register::Rax, value);
        require(machine.Run(0x1030, 0x1033, 10) == Cpu::StopReason::Address, "Actual x86 memory store did not finish");
    }
};

void allocationAndOutputGates() {
    {
        Session wide(0x100004000ULL);
        require(wide.call("pO96TwzOm5E") == 0x100004000ULL,
                "Direct size gate truncated its uint64 result above 4 GiB");
    }
    Session session;
    require(session.call("pO96TwzOm5E") == capacity, "Direct size gate did not return the configured uint64 capacity");
    session.write(0x3000, std::uint64_t{0x1122334455667788});
    require(session.call("C0f7TJcbfac", {0,capacity,0,0x3000,0x5ffc}) == fault &&
            session.read<std::uint64_t>(0x3000) == 0x1122334455667788,
            "Available gate wrote its first output before validating the entire second output");
    require(session.call("C0f7TJcbfac", {0,capacity,0,0x3000,0x3010}) == 0 &&
            session.read<std::uint64_t>(0x3000) == 0 && session.read<std::uint64_t>(0x3010) == capacity,
            "Available gate lost its fourth/fifth SysV output arguments or changed free capacity");
    require(session.call("rTXw65xmLIA", {0,capacity,page,0,0,0x5ffc}) == fault,
            "Allocate gate failed to validate its sixth SysV argument before consuming physical memory");
    require(session.call("B+vc2AO2Zrc", {page,0,0,0x3020}) == 0 && session.read<std::uint64_t>(0x3020) == 0,
            "Rejected allocate consumed physical memory or main allocate used the wrong output register");
    require(session.call("MBuItvba6z8", {0,page}) == 0, "Release gate did not retire an unmapped physical allocation");
    require(session.call("rTXw65xmLIA", {page,7*page,2*page,2*page,0xdeadbeef00000000ULL,0x3020}) == 0 &&
            session.read<std::uint64_t>(0x3020) == 2*page,
            "Allocate lost signed search bounds, alignment or low32 memory type marshalling");
    require(session.call("rTXw65xmLIA", {~0ULL,capacity,page,0,0,0x3020}) == invalid &&
            session.read<std::uint64_t>(0x3020) == 2*page,
            "Negative search start did not produce a full signed EINVAL without changing output");
    require(session.call("MBuItvba6z8", {2*page,2*page}) == 0, "Allocated extent did not release through actual guest gate");
}

void mappedAliasesAndQuery() {
    Session session;
    require(session.call("rTXw65xmLIA", {page,capacity,2*page,2*page,0,0x3000}) == 0,
            "Cannot allocate independent physical memory for gate ABI proof");
    require(session.read<std::uint64_t>(0x3000) == 0x8000, "Physical allocator did not honor the independently aligned search interval");
    constexpr std::uint64_t address = 0x1000000000;
    constexpr std::uint64_t alias = 0x1000020000;
    const auto beforeMap = session.transactions;
    session.write(0x3010, address);
    require(session.call("L-Q3LEjIbgA", {0x3010,2*page,2,0x90,0x8000,0x4001}) == invalid &&
            session.read<std::uint64_t>(0x3010) == address && session.transactions == beforeMap,
            "MapDirect ignored the sixth alignment argument or mutated before its validation");
    require(session.call("L-Q3LEjIbgA", {0x3010,2*page,0xabcdef0100000002ULL,0xabcdef0100000090ULL,0x8000,0x10000}) == 0 &&
            session.read<std::uint64_t>(0x3010) == address && session.transactions == beforeMap + 1,
            "MapDirect lost low32 protection/flags or failed to commit synchronously");
    session.store(address + page, 0xfedcba9876543210ULL);
    require(session.load(address + page) == 0xfedcba9876543210ULL, "Protection2 did not permit an actual guest read after guest write");
    std::array<std::byte,80> query;
    query.fill(std::byte{0xa5});
    session.machine.Write(0x3100, query);
    require(session.call("rVjRvHJ0X6c", {address,0xdeadbeef00000000ULL,0x3100,80}) == 0,
            "VirtualQuery gate rejected valid low32 flags or 72-byte ABI output");
    std::array<std::byte,72> expected{};
    expected[4] = std::byte{0x10};
    expected[9] = std::byte{0x80}; expected[12] = std::byte{0x10};
    expected[17] = std::byte{0x80};
    expected[24] = std::byte{2}; expected[32] = std::byte{0x12};
    const auto actual = session.read<std::array<std::byte,80>>(0x3100);
    for (std::size_t index = 0; index < expected.size(); ++index)
        require(actual[index] == expected[index], "VirtualQuery did not encode independent 72-byte PS5 field offsets and flags");
    for (std::size_t index = 72; index < actual.size(); ++index)
        require(actual[index] == std::byte{0xa5}, "VirtualQuery wrote beyond its 72-byte ABI object");
    std::array<std::byte,64> prefix;
    prefix.fill(std::byte{0x5a});
    session.machine.Write(0x5fc0, prefix);
    require(session.call("rVjRvHJ0X6c", {address,0,0x5fc0,72}) == fault &&
            session.read<std::array<std::byte,64>>(0x5fc0) == prefix,
            "VirtualQuery failed to check the full cross-page output span");
    require(session.call("rVjRvHJ0X6c", {address,0,0x3100,71}) == invalid,
            "VirtualQuery did not sign-extend short-object EINVAL");
    require(session.call("rVjRvHJ0X6c", {address,2,0x3100,72}) == invalid,
            "VirtualQuery with unsupported flags escaped as a host exception instead of signed EINVAL");
    session.write(0x3020, alias);
    require(session.call("L-Q3LEjIbgA", {0x3020,page,3,0x90,0xc000,0}) == 0,
            "MapDirect alias lost the fifth physical offset argument");
    require(session.load(alias) == 0xfedcba9876543210ULL, "Actual guest alias read did not observe the original physical page");
    session.store(alias, 0x1122334455667788ULL);
    require(session.load(address + page) == 0x1122334455667788ULL, "Actual guest alias write did not cohere synchronously");
    require(session.call("vSMAm3cxYTY", {alias,page,0x123400000001ULL}) == 0,
            "Mprotect did not use low32 protection argument");
    rejects([&] { session.store(alias, 7); }, "protected write");
    require(session.load(address + page) == 0x1122334455667788ULL, "Mprotect changed the permissions or bytes of a retained peer alias");
    require(session.call("cQke9UuBQOk", {alias,page}) == 0, "Unmap gate failed to retire the alias");
    // MEM-11: releasing a still-mapped allocation unmaps its remaining aliases instead of failing.
    require(session.call("MBuItvba6z8", {0x8000,2*page}) == 0, "Release of a mapped physical allocation was refused");
    rejects([&] { session.load(alias); }, "unmapped read");
    rejects([&] { session.load(address + page); }, "unmapped read");
}

void flexibleReservedAndScope() {
    Session session;
    constexpr std::uint64_t flexible = 0x1000040000;
    constexpr std::uint64_t reserved = 0x1000080000;
    session.write(0x3000, flexible);
    require(session.call("IWIBBdTHit4", {0x5ffc,page,3,0}) == fault && session.transactions == 0,
            "Flexible gate mutated memory before checking its entire in/out slot");
    require(session.call("IWIBBdTHit4", {0x3000,page,3,0x90}) == 0 && session.read<std::uint64_t>(0x3000) == flexible,
            "Flexible memory gate failed checked address marshalling");
    session.store(flexible, 0x1234);
    session.write(0x3010, flexible);
    require(session.call("IWIBBdTHit4", {0x3010,page,3,0x90}) == exists && session.load(flexible) == 0x1234,
            "No-overwrite flexible gate did not return signed EEXIST without changing existing data");
    require(session.call("IWIBBdTHit4", {0x3010,page,0x400,0}) == invalid &&
            session.call("IWIBBdTHit4", {0x3010,page,3,2}) == invalid,
            "Unsupported protection bits or mapping flags escaped as a host exception instead of signed EINVAL");
    require(session.load(flexible) == 0x1234, "Unsupported flags or protections changed the old mapping");
    session.write(0x3020, reserved);
    require(session.call("7oxv3PPCumo", {0x3020,page,0x90,0x10000}) == 0 && session.read<std::uint64_t>(0x3020) == reserved,
            "Reserve gate lost its fourth alignment argument or address output");
    // MEM-10: a fixed no-overwrite map lands in a reservation; a plain fixed map replaces.
    session.write(0x3010, reserved);
    require(session.call("IWIBBdTHit4", {0x3010,page,3,0x90}) == 0 && session.read<std::uint64_t>(0x3010) == reserved,
            "No-overwrite mapping refused a real guest reservation");
    session.store(reserved, 0x4321);
    require(session.call("IWIBBdTHit4", {0x3010,page,3,0x90}) == exists && session.load(reserved) == 0x4321,
            "No-overwrite mapping replaced committed memory");
    require(session.call("IWIBBdTHit4", {0x3010,page,3,0x10}) == 0 && session.load(reserved) == 0,
            "Fixed replacement did not install fresh memory");
    require(session.call("cQke9UuBQOk", {reserved,page}) == 0 && session.call("cQke9UuBQOk", {flexible,page}) == 0,
            "Munmap gate did not retire flexible memory and a real reservation");
    const auto qualified = import("pO96TwzOm5E");
    const auto gate = session.imports->Resolve(qualified);
    require(gate == session.imports->Resolve(qualified), "Repeated complete memory identity changed its gate");
    auto local = qualified;
    local.LibraryId = 23; local.ModuleId = 41;
    const auto localGate = session.imports->Resolve(local);
    require(localGate && localGate != gate && session.callGate(*localGate) == capacity,
            "Memory provider discarded importer-local IDs or used them as global scopes");
    for (unsigned mismatch = 0; mismatch < 5; ++mismatch) {
        auto wrong = qualified;
        switch (mismatch) {
        case 0: wrong.LibraryName = "other"; break;
        case 1: wrong.ModuleName = "other"; break;
        case 2: wrong.LibraryVersion = 2; break;
        case 3: wrong.ModuleMajor = 2; break;
        case 4: wrong.ModuleMinor = 2; break;
        }
        rejects([&] { session.imports->Resolve(wrong); }, "scope/version");
    }
    require(!session.imports->Resolve(import("959qrazPIrg")), "Memory provider stole the independent bootstrap GetProcParam service");
    session.imports.reset();
    rejects([&] { session.callGate(*gate); }, "runtime has expired");
}
}

void unsupportedRequestsReachTheGuest() {
    // #216: an unsupported but legal memory request must arrive as a signed SCE kernel error so the
    // title can handle it. This is what stopped the reference test title inside dependency
    // initialisation: libc.prx asked for protection bits 0xF2 and the exception escaped the gate.
    Session session;
    constexpr std::uint64_t address = 0x1000000000;
    session.write(0x3010, address);
    const auto before = session.transactions;
    require(session.call("rTXw65xmLIA", {0, capacity, page, 3, 0xffffffff, 0x3020}) == invalid &&
            session.transactions == before,
            "sceKernelAllocateDirectMemory with a negative memory type aborted the run instead of returning EINVAL");
    require(session.call("L-Q3LEjIbgA", {0x3010, page, 3, 0x1000, 0x8000, 0}) == invalid &&
            session.transactions == before,
            "sceKernelMapDirectMemory with an unsupported flag bit aborted the run instead of returning EINVAL");
    require(session.call("rVjRvHJ0X6c", {address, 2, 0x3100, 72}) == invalid &&
            session.transactions == before,
            "sceKernelVirtualQuery with flags 2 aborted the run instead of returning EINVAL");
    // Protection bits outside the documented 0x3F7 set, and their low32 marshalling rule. (The
    // reference title's 0xF2 request is supported; see libkernelMemorySurface.)
    require(session.call("vSMAm3cxYTY", {address, page, 0x400}) == invalid &&
            session.transactions == before,
            "sceKernelMprotect with undocumented protection bits aborted the run instead of EINVAL");
    require(session.call("vSMAm3cxYTY", {address, page, 0xabcdef0100000400ULL}) == invalid &&
            session.transactions == before,
            "the undocumented protection request lost its low32 marshalling or mutated guest memory");
    // No host abort means no lost state: the same gate still serves a supported request.
    require(session.call("pO96TwzOm5E") == capacity && session.transactions == before,
            "memory gates stopped answering after an unsupported request was converted to an error code");
}

// #282: the rest of the libkernel memory surface, through actual x86 SysV calls. NIDs are the
// published values for the names in the comments, independent of the provider's own table.
void libkernelMemorySurface() {
    Session session;
    constexpr std::uint64_t base = 0x1000000000;
    constexpr std::uint64_t enomem = 0xffffffff8002000cULL;
    constexpr std::uint64_t eacces = 0xffffffff8002000dULL;
    constexpr std::uint64_t enoent = 0xffffffff80020002ULL;
    const auto text = [&](std::uint64_t address, const char* value) {
        session.machine.Write(address, std::as_bytes(std::span(value, std::char_traits<char>::length(value) + 1)));
    };
    const auto name = [&](std::uint64_t address) {
        require(session.call("rVjRvHJ0X6c", {address, 0, 0x3100, 72}) == 0, "VirtualQuery of a named range failed");
        const auto bytes = session.read<std::array<char, 72>>(0x3100);
        return std::string(bytes.data() + 36);
    };
    // sceKernelMapNamedFlexibleMemory with the reference title's 0xF2 protection request.
    text(0x3200, "title-heap");
    session.write(0x3000, base);
    require(session.call("mL8NDH86iQI", {0x3000, page, 0xF2, 0x10, 0x3200}) == 0 && session.read<std::uint64_t>(0x3000) == base,
            "sceKernelMapNamedFlexibleMemory did not map with protection 0xF2");
    require(name(base) == "title-heap" && session.read<std::uint32_t>(0x3100 + 24) == 0xF2,
            "Named flexible memory lost its name or raw protection");
    session.store(base, 0x5a5a);
    require(session.load(base) == 0x5a5a, "0xF2 mapping is not CPU read/write");
    const auto before = session.transactions;
    session.write(0x3000, base + page);
    require(session.call("mL8NDH86iQI", {0x3000, page, 3, 0x10, 0x9000}) == fault && session.transactions == before,
            "Unreadable range name was not rejected before mapping");
    // sceKernelMapNamedSystemFlexibleMemory, sceKernelSetVirtualRangeName.
    text(0x3220, "system");
    require(session.call("kc+LEEIYakc", {0x3000, page, 3, 0x10, 0x3220}) == 0 && name(base + page) == "system",
            "sceKernelMapNamedSystemFlexibleMemory did not map a named range");
    text(0x3240, "renamed");
    require(session.call("DGMG3JshrZU", {base, page, 0x3240}) == 0 && name(base) == "renamed",
            "sceKernelSetVirtualRangeName did not rename the range");
    // sceKernelAvailableFlexibleMemorySize / sceKernelConfiguredFlexibleMemorySize.
    require(session.call("n1-v6FgU7MQ", {0x3020}) == 0 && session.call("aNz11fnnzi4", {0x3028}) == 0 &&
            session.read<std::uint64_t>(0x3020) == 448ULL << 20 &&
            session.read<std::uint64_t>(0x3028) == (448ULL << 20) - 2 * page,
            "Flexible size queries do not report the configured budget and its use");
    // sceKernelReleaseFlexibleMemory returns the budget.
    require(session.call("teiItL2boFw", {base + page, page}) == 0 && session.call("aNz11fnnzi4", {0x3028}) == 0 &&
            session.read<std::uint64_t>(0x3028) == (448ULL << 20) - page, "sceKernelReleaseFlexibleMemory did not unmap");
    // sceKernelAllocateDirectMemory type 3, sceKernelDirectMemoryQuery, sceKernelGetDirectMemoryType.
    require(session.call("rTXw65xmLIA", {0, capacity, 2 * page, page, 3, 0x3030}) == 0 && session.read<std::uint64_t>(0x3030) == 0,
            "Direct memory type 3 was refused");
    require(session.call("BHouLQzh0X0", {page, 0, 0x3300, 24}) == 0 && session.read<std::int64_t>(0x3300) == 0 &&
            session.read<std::int64_t>(0x3308) == 2 * page && session.read<std::int32_t>(0x3310) == 3,
            "sceKernelDirectMemoryQuery did not report the allocation and its type");
    require(session.call("BHouLQzh0X0", {4 * page, 1, 0x3300, 24}) == eacces && session.call("BHouLQzh0X0", {0, 0, 0x3300, 8}) == invalid,
            "sceKernelDirectMemoryQuery invented an allocation or accepted a short output");
    require(session.call("BC+OG5m9+bw", {page, 0x3320, 0x3328, 0x3330}) == 0 && session.read<std::int32_t>(0x3320) == 3 &&
            session.read<std::int64_t>(0x3328) == 0 && session.read<std::int64_t>(0x3330) == 2 * page &&
            session.call("BC+OG5m9+bw", {4 * page, 0x3320, 0x3328, 0x3330}) == enoent,
            "sceKernelGetDirectMemoryType did not report the allocation");
    // sceKernelMapNamedDirectMemory: the name is the seventh argument, on the stack.
    session.write(0x3000, base + 4 * page);
    session.write(0x4ff0, std::uint64_t{0x3200});
    require(session.call("NcaWUxfMNIQ", {0x3000, page, 0x32, 0x10, 0, 0}) == 0 && name(base + 4 * page) == "title-heap",
            "sceKernelMapNamedDirectMemory lost its stack name argument");
    // sceKernelMapDirectMemory2: type, protection, flags, offset, alignment on the stack.
    session.write(0x3000, base + 5 * page);
    session.write(0x4ff0, std::uint64_t{0});
    require(session.call("BQQniolj9tQ", {0x3000, page, 10, 3, 0x10, page}) == 0, "sceKernelMapDirectMemory2 did not map");
    require(session.call("rVjRvHJ0X6c", {base + 5 * page, 0, 0x3100, 72}) == 0 && session.read<std::int32_t>(0x3100 + 28) == 10,
            "sceKernelMapDirectMemory2 lost its memory type");
    // sceKernelMtypeprotect, sceKernelQueryMemoryProtection.
    require(session.call("9bfdLIyuwCY", {base + 5 * page, page, 12, 1}) == 0 &&
            session.call("WFcfL2lzido", {base + 5 * page, 0x3340, 0x3348, 0x3350}) == 0 &&
            session.read<std::uint64_t>(0x3340) == base + 5 * page && session.read<std::uint64_t>(0x3348) == base + 6 * page &&
            session.read<std::int32_t>(0x3350) == 1, "sceKernelMtypeprotect/QueryMemoryProtection disagree");
    // sceKernelReleaseDirectMemory implicitly unmaps live aliases; the checked variant reports holes.
    require(session.call("MBuItvba6z8", {0, 2 * page}) == 0, "Release of mapped direct memory was refused");
    rejects([&] { session.load(base + 4 * page); }, "unmapped read");
    require(session.call("hwVSPCmp5tM", {0, page}) == enoent && session.call("hwVSPCmp5tM", {0, 0}) == 0,
            "sceKernelCheckedReleaseDirectMemory did not report the unallocated range");
    // sceKernelBatchMap: map flexible, protect, then stop at an invalid operation and report the count.
    struct Entry { std::uint64_t start, offset, length; std::uint8_t protection, type; std::uint16_t reserved; std::int32_t operation; };
    const std::array<Entry, 3> batch{{{base + 8 * page, 0, page, 3, 0, 0, 3}, {base + 8 * page, 0, page, 1, 0, 0, 2},
                                      {base + 9 * page, 0, page, 3, 0, 0, 9}}};
    session.write(0x3400, batch);
    require(session.call("2SKEx6bSq-4", {0x3400, 3, 0x3460}) == invalid && session.read<std::int32_t>(0x3460) == 2,
            "sceKernelBatchMap did not stop at the invalid entry with the processed count");
    require(session.call("WFcfL2lzido", {base + 8 * page, 0, 0, 0x3350}) == 0 && session.read<std::int32_t>(0x3350) == 1,
            "sceKernelBatchMap entries were not applied in order");
    require(session.call("kBJzF8x4SyE", {0x3400, 1, 0x3460, 0x90}) == exists && session.read<std::int32_t>(0x3460) == 0,
            "sceKernelBatchMap2 ignored its NO_OVERWRITE flags");
    // Memory pools.
    require(session.call("bvD+95Q6asU", {0x3500, 16}) == 0 && session.read<std::array<std::int32_t, 4>>(0x3500) == std::array<std::int32_t, 4>{},
            "Empty pool reported blocks");
    require(session.call("qCSfqDILlns", {0, capacity, 4 * page, 0, 0x3520}) == 0, "sceKernelMemoryPoolExpand failed");
    require(session.call("pU-QydtGcGY", {base + 16 * page, 8 * page, 4 * page, 0x10, 0x3528}) == 0 &&
            session.read<std::uint64_t>(0x3528) == base + 16 * page, "sceKernelMemoryPoolReserve did not report its address");
    require(session.call("Vzl66WmfLvk", {base + 16 * page, 4 * page, 0, 3, 0}) == 0, "sceKernelMemoryPoolCommit failed");
    session.store(base + 16 * page, 0x77);
    require(session.load(base + 16 * page) == 0x77, "Committed pool memory is not guest read/write");
    require(session.call("Vzl66WmfLvk", {base + 20 * page, 4 * page, 0, 3, 0}) == enomem,
            "Pool commit beyond the expanded pool did not return ENOMEM");
    require(session.call("bvD+95Q6asU", {0x3500, 16}) == 0 && session.read<std::array<std::int32_t, 4>>(0x3500)[2] == 1,
            "Pool block stats lost the committed block");
    require(session.call("LXo1tpFqJGs", {base + 16 * page, 4 * page, 0}) == 0, "sceKernelMemoryPoolDecommit failed");
    rejects([&] { session.load(base + 16 * page); }, "unmapped read");
    // sceKernelMemoryPoolBatch: commit, protect, then stop at the unsupported move with the count.
    struct PoolEntry { std::uint32_t operation, flags; std::uint64_t address, length; std::uint8_t protection, type; std::uint8_t pad[6]; };
    const std::array<PoolEntry, 3> pool{{{1, 0, base + 16 * page, 4 * page, 3, 0, {}}, {3, 0, base + 16 * page, 4 * page, 1, 0, {}},
                                         {5, 0, base + 16 * page, 4 * page, 0, 0, {}}}};
    session.write(0x3600, pool);
    require(session.call("YN878uKRBbE", {0x3600, 3, 0x3660, 0}) == invalid && session.read<std::int32_t>(0x3660) == 2,
            "sceKernelMemoryPoolBatch did not stop at the unsupported entry with the processed count");
    require(session.load(base + 16 * page) == 0, "Pool batch commit did not back the reservation");
    rejects([&] { session.store(base + 16 * page, 1); }, "protected write");
    // sceKernelMlock / sceKernelMunlock validate the range.
    require(session.call("3k6kx-zOOSQ", {base, page}) == 0 && session.call("xQIIfJ860sk", {base, page}) == 0 &&
            session.call("3k6kx-zOOSQ", {base + 30 * page, page}) == enomem, "sceKernelMlock did not validate its range");
}

// POSIX aliases return -1 and set the active thread's errno; they are also exported through libScePosix.
void posixAliases() {
    Session session;
    session.imports->SetErrnoLocation([] { return std::uint64_t{0x3600}; });
    session.write(0x3600, std::int32_t{0});
    const auto posix = [&](const char* nid, std::array<std::uint64_t, 6> arguments) {
        auto value = import(nid);
        value.LibraryName = "libScePosix";
        const auto gate = session.imports->Resolve(value);
        require(gate.has_value(), "POSIX memory alias did not resolve through libScePosix");
        return session.callGate(*gate, arguments);
    };
    const auto address = posix("BPE9s9vQQXo", {0, page + 1, 3, 0x1002, ~0ULL, 0});
    require(address >= 0x1000000000 && address % page == 0, "mmap did not map anonymous memory");
    session.store(address + page, 0x99);
    require(session.load(address + page) == 0x99, "mmap did not round its length up to whole pages");
    require(posix("BPE9s9vQQXo", {0, page, 3, 0x0002, 3, 0}) == ~0ULL && session.read<std::int32_t>(0x3600) == 45,
            "File-backed mmap did not fail with MAP_FAILED and errno");
    require(posix("BPE9s9vQQXo", {0, page, 8, 0x1002, ~0ULL, 0}) == ~0ULL && session.read<std::int32_t>(0x3600) == 22,
            "mmap accepted a non-POSIX protection");
    require(posix("YQOfxL4QfeU", {address, 1, 1}) == 0, "mprotect failed");
    rejects([&] { session.store(address, 1); }, "protected write");
    require(posix("YQOfxL4QfeU", {0x7000000000, page, 1}) == ~0ULL && session.read<std::int32_t>(0x3600) == 13,
            "mprotect of an unmapped range did not set errno");
    require(posix("UqDGjXA5yUM", {address, 2 * page + 1}) == 0, "munmap failed over a rounded length");
    rejects([&] { session.load(address + page); }, "unmapped read");
    require(session.call("PGhQHd-dzv8", {0, page, 3, 0x1002, ~0ULL, 0}) == invalid,
            "sceKernelMmap did not validate its stack result pointer");
    session.write(0x4ff0, std::uint64_t{0x3610});
    require(session.call("PGhQHd-dzv8", {0, page, 3, 0x1002, ~0ULL, 0}) == 0 && session.read<std::uint64_t>(0x3610) >= 0x1000000000,
            "sceKernelMmap did not write its mapping through the seventh argument");
    // A fixed read-only map over the page that holds the result pointer is refused before it commits.
    const auto mapped = session.read<std::uint64_t>(0x3610);
    session.store(mapped, 0x31);
    session.write(0x4ff0, mapped + 8);
    require(session.call("PGhQHd-dzv8", {mapped, page, 1, 0x1012, ~0ULL, 0}) == fault && session.load(mapped) == 0x31,
            "sceKernelMmap replaced memory before discovering its result pointer would become read-only");
    // Without an active guest thread the alias still fails cleanly; only errno is left alone.
    session.imports->SetErrnoLocation([]() -> std::uint64_t { throw std::runtime_error("there is no active guest thread"); });
    require(posix("BPE9s9vQQXo", {0, page, 8, 0x1002, ~0ULL, 0}) == ~0ULL,
            "A missing errno slot turned the POSIX error into a host abort");
}

int main() {
    try {
        allocationAndOutputGates();
        mappedAliasesAndQuery();
        flexibleReservedAndScope();
        unsupportedRequestsReachTheGuest();
        libkernelMemorySurface();
        posixAliases();
        std::cout << "PASS typed memory NID gates, actual x86 SysV calls, full signed errors, checked outputs, 72-byte query, synchronous aliases and unsupported requests returned as SCE error codes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
