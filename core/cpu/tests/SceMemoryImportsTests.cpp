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
constexpr std::uint64_t busy = 0xffffffff80020010ULL;        // errno 16 EBUSY
constexpr std::uint64_t unsupported = 0xffffffff8002002dULL; // errno 45 ENOTSUP/EOPNOTSUPP

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
    require(session.call("MBuItvba6z8", {0x8000,2*page}) == busy,
            "Release of a mapped physical allocation escaped as a host exception instead of signed EBUSY");
    require(session.load(alias) == 0x1122334455667788ULL, "Unsupported live release silently removed its physical aliases");
    require(session.call("cQke9UuBQOk", {alias,page}) == 0 && session.call("cQke9UuBQOk", {address,2*page}) == 0 &&
            session.call("MBuItvba6z8", {0x8000,2*page}) == 0, "Unmap/release gates failed to retire both physical aliases");
    rejects([&] { session.load(alias); }, "unmapped read");
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
    require(session.call("IWIBBdTHit4", {0x3010,page,0x40,0}) == invalid &&
            session.call("IWIBBdTHit4", {0x3010,page,3,2}) == invalid,
            "Unsupported protection bits or mapping flags escaped as a host exception instead of signed EINVAL");
    require(session.load(flexible) == 0x1234, "Unsupported flags or protections changed the old mapping");
    session.write(0x3020, reserved);
    require(session.call("7oxv3PPCumo", {0x3020,page,0x90,0x10000}) == 0 && session.read<std::uint64_t>(0x3020) == reserved,
            "Reserve gate lost its fourth alignment argument or address output");
    session.write(0x3010, reserved);
    require(session.call("IWIBBdTHit4", {0x3010,page,3,0x90}) == exists,
            "No-overwrite mapping incorrectly treated a real guest reservation as free");
    require(session.call("IWIBBdTHit4", {0x3010,page,3,0x10}) == unsupported,
            "Fixed replacement over a real reservation escaped as a host exception instead of signed ENOTSUP");
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
    require(session.call("rTXw65xmLIA", {0, capacity, page, 3, 0, 0x3020}) == invalid &&
            session.transactions == before,
            "sceKernelAllocateDirectMemory with memory type 3 aborted the run instead of returning EINVAL");
    require(session.call("L-Q3LEjIbgA", {0x3010, page, 3, 0x1000, 0x8000, 0}) == invalid &&
            session.transactions == before,
            "sceKernelMapDirectMemory with an unsupported flag bit aborted the run instead of returning EINVAL");
    require(session.call("rVjRvHJ0X6c", {address, 2, 0x3100, 72}) == invalid &&
            session.transactions == before,
            "sceKernelVirtualQuery with flags 2 aborted the run instead of returning EINVAL");
    // The exact live request from the reference title's libc.prx, and its low32 marshalling rule.
    require(session.call("vSMAm3cxYTY", {address, page, 0xF2}) == invalid &&
            session.transactions == before,
            "sceKernelMprotect with the title's 0xF2 protection bits aborted the run instead of EINVAL");
    require(session.call("vSMAm3cxYTY", {address, page, 0xabcdef01000000f2ULL}) == invalid &&
            session.transactions == before,
            "the 0xF2 protection request lost its low32 marshalling or mutated guest memory");
    // No host abort means no lost state: the same gate still serves a supported request.
    require(session.call("pO96TwzOm5E") == capacity && session.transactions == before,
            "memory gates stopped answering after an unsupported request was converted to an error code");
}

int main() {
    try {
        allocationAndOutputGates();
        mappedAliasesAndQuery();
        flexibleReservedAndScope();
        unsupportedRequestsReachTheGuest();
        std::cout << "PASS typed memory NID gates, actual x86 SysV calls, full signed errors, checked outputs, 72-byte query, synchronous aliases and unsupported requests returned as SCE error codes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
