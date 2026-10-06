#include <cpu/SceLibcBootstrapImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what()); return;
    }
    throw std::runtime_error(std::string("Missing bootstrap rejection: ") + expected);
}
Cpu::SceImport qualified(const char* nid, bool trace = false) {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = trace ? "libSceLibcInternalExt" : "libkernel";
    value.ModuleName = trace ? "libSceLibcInternal" : "libkernel";
    value.LibraryId = trace ? 1 : 44; value.ModuleId = trace ? 2 : 24;
    value.LibraryVersion = 1; value.ModuleMajor = 1; value.ModuleMinor = 1;
    return value;
}
struct Session {
    Cpu::Machine machine;
    Cpu::SceLibcBootstrapImports imports{machine};
    Session() {
        machine.Map(0x1000, 4096, rx); machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw); machine.Map(0x4000, 4096, rw); machine.Map(0x6000, 4096, rw);
        constexpr std::array<std::uint8_t, 17> caller{
            0xff, 0x15, 0xfa, 0x0f, 0, 0, 0x48, 0x89, 0x05, 0x03, 0x10, 0, 0, 0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(caller)));
        constexpr std::array<std::uint8_t, 10> object{0x48, 0x8b, 0x05, 0x79, 0x0f, 0, 0, 0x48, 0x8b, 0x00};
        machine.Write(0x1080, std::as_bytes(std::span(object)));
        constexpr std::array<std::uint8_t, 7> nested{0x48, 0x8b, 0x40, 0x38, 0x48, 0x8b, 0x00};
        machine.Write(0x10a0, std::as_bytes(std::span(nested)));
        constexpr std::array<std::uint8_t, 3> store{0x48, 0x89, 0x37};
        machine.Write(0x10c0, std::as_bytes(std::span(store)));
    }
    std::uint64_t invoke(std::uint64_t gate, std::uint64_t argument = 0) {
        machine.Write(0x2000, std::as_bytes(std::span(&gate, 1)));
        machine.Set(Register::Rdi, argument); machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rbx, 0x123456789abcdeff);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address && machine.Get(Register::Rsp) == 0x4ff0 &&
                machine.Get(Register::Rbx) == 0x123456789abcdf00, "Bootstrap gate corrupted actual guest CALL/RET continuation");
        return qword(0x2010);
    }
    std::uint64_t object(std::uint64_t address) {
        machine.Write(0x2000, std::as_bytes(std::span(&address, 1)));
        require(machine.Run(0x1080, 0x108a, 100) == Cpu::StopReason::Address, "Guest object GOT dereference did not complete");
        return machine.Get(Register::Rax);
    }
    void store(std::uint64_t address, std::uint64_t value) {
        machine.Set(Register::Rdi, address); machine.Set(Register::Rsi, value);
        require(machine.Run(0x10c0, 0x10c3, 100) == Cpu::StopReason::Address, "Actual guest trace qword write failed");
    }
    std::uint64_t resolve(const char* nid, bool trace = false) { return imports.Resolve(qualified(nid, trace)).value(); }
    std::uint64_t qword(std::uint64_t address) {
        std::uint64_t value = 0; machine.Read(address, std::as_writable_bytes(std::span(&value, 1))); return value;
    }
    std::vector<std::uint8_t> bytes(std::uint64_t address, std::size_t count) {
        std::vector<std::uint8_t> value(count); machine.Read(address, std::as_writable_bytes(std::span(value))); return value;
    }
    void fill(std::uint64_t address, std::size_t count) {
        const std::vector<std::uint8_t> value(count, 0xa7); machine.Write(address, std::as_bytes(std::span(value)));
    }
};

void processParameters() {
    Session session;
    const auto gate = session.resolve("959qrazPIrg");
    require(session.invoke(gate) == 0, "Absent process parameters returned a fabricated guest object");
    rejects([&] { session.imports.SetProcessParameters(0x6ff0, 96); }, "Guest access denied");
    require(session.invoke(gate) == 0, "Rejected process-parameter span became active");
    const std::uint64_t outerSize = 96, nestedAddress = 0x3200, nestedSize = 168;
    session.machine.Write(0x3000, std::as_bytes(std::span(&outerSize, 1)));
    session.machine.Write(0x3038, std::as_bytes(std::span(&nestedAddress, 1)));
    session.machine.Write(0x3200, std::as_bytes(std::span(&nestedSize, 1)));
    const auto original = session.bytes(0x3000, 96);
    session.machine.Protect(0x3000, 4096, Permission::Read);
    session.imports.SetProcessParameters(0x3000, 96);
    require(session.invoke(gate) == 0x3000, "Process-parameter gate did not return the configured guest address");
    require(session.machine.Run(0x10a0, 0x10a4, 100) == Cpu::StopReason::Address && session.machine.Get(Register::Rax) == 0x3200,
            "Actual guest did not read the preserved nested process-parameter pointer");
    require(session.machine.Run(0x10a4, 0x10a7, 100) == Cpu::StopReason::Address && session.machine.Get(Register::Rax) == 168,
            "Nested process parameters could not be dereferenced by actual x86 code");
    require(session.bytes(0x3000, 96) == original, "Process parameter configuration changed the mapped object");
    session.imports.SetProcessParameters(0x3000, 96);
    rejects([&] { session.imports.SetProcessParameters(0x3200, 96); }, "cannot replace an active span");
}

void callbackRegistration() {
    Session session;
    const auto gate = session.resolve("p5EcQeEeJAE");
    const std::array<std::uint64_t, 10> callbacks{0x1020, 0x1040, 0, 0, 0, 0, 0x1060, 0, 0x1080, 0};
    require(!session.imports.HeapCallbacks(), "Heap callbacks were fabricated before registration");
    session.machine.Write(0x6fd8, std::as_bytes(std::span(callbacks).first(5)));
    for (const auto address : std::array<std::uint64_t, 4>{0, 0x9000, 0x6fd8, std::numeric_limits<std::uint64_t>::max() - 1}) {
        rejects([&] { (void)session.invoke(gate, address); }, address == 0 || address > 0x9000 ? "guest span is invalid" : "Guest access denied");
        require(!session.imports.HeapCallbacks(), "Rejected table partially captured active callbacks");
    }
    session.machine.Write(0x3000, std::as_bytes(std::span(callbacks)));
    auto invalid = callbacks; invalid[6] = 0x3000;
    session.machine.Write(0x3000, std::as_bytes(std::span(invalid)));
    rejects([&] { (void)session.invoke(gate, 0x3000); }, "permission");
    require(!session.imports.HeapCallbacks(), "Invalid optional callback changed registration state");
    for (const auto index : {0, 1}) {
        invalid = callbacks; invalid[index] = 0;
        session.machine.Write(0x3000, std::as_bytes(std::span(invalid)));
        rejects([&] { (void)session.invoke(gate, 0x3000); }, "require malloc and free");
        require(!session.imports.HeapCallbacks(), "Missing mandatory callback partially captured a table");
    }
    session.machine.Write(0x3000, std::as_bytes(std::span(callbacks)));
    session.machine.Protect(0x3000, 4096, Permission::Read);
    (void)session.invoke(gate, 0x3000);
    require(session.imports.HeapCallbacks().value() == callbacks, "Registration did not retain all ten guest callback identities");
    (void)session.invoke(gate, 0x3000);
    session.machine.Protect(0x3000, 4096, rw);
    auto replacement = callbacks; replacement[1] = 0x1060;
    session.machine.Write(0x3000, std::as_bytes(std::span(replacement)));
    require(session.imports.HeapCallbacks().value() == callbacks, "Captured callbacks alias mutable registration input");
    rejects([&] { (void)session.invoke(gate, 0x3000); }, "cannot replace an active table");
    require(session.imports.HeapCallbacks().value() == callbacks, "Rejected replacement changed active callbacks");
}

void objectsAndTrace() {
    Session session;
    const auto guard = session.resolve("f7uOxY9mM1U"), program = session.resolve("djxxOmW6-aw");
    const auto guardValue = session.object(guard), nameAddress = session.object(program);
    require(guardValue != 0, "Guest stack guard was zero or not an eight-byte readable object");
    require(session.bytes(nameAddress, 10) == std::vector<std::uint8_t>{'e', 'b', 'o', 'o', 't', '.', 'b', 'i', 'n', 0},
            "Program-name object did not point to owned eboot.bin guest C-string storage");
    session.machine.Set(Register::Rax, nameAddress);
    require(session.machine.Run(0x10a4, 0x10a7, 100) == Cpu::StopReason::Address && session.machine.Get(Register::Rax) == 0x69622e746f6f6265ULL,
            "Actual x86 code could not dereference the name pointer returned through the imported object");
    for (const auto nid : {"f7uOxY9mM1U", "djxxOmW6-aw"}) {
        auto fromLibc = qualified(nid); fromLibc.LibraryId = 0; fromLibc.ModuleId = 1;
        require(session.imports.Resolve(fromLibc).value() == session.resolve(nid), "Local importer IDs duplicated the same runtime object");
    }
    const auto gate = session.resolve("NWtTN10cJzE", true);
    session.fill(0x3000, 48);
    const std::array<std::uint64_t, 4> input{32, 0x7654321000000001ULL, 0, 0};
    session.machine.Write(0x3008, std::as_bytes(std::span(input)));
    (void)session.invoke(gate, 0x3008);
    const auto mask = session.qword(0x3018), table = session.qword(0x3020);
    require(session.qword(0x3008) == 32 && session.qword(0x3010) == 1, "Trace changed size/low input field or failed to clear output flag12");
    require(session.bytes(0x3000, 8) == std::vector<std::uint8_t>(8, 0xa7) && session.bytes(0x3028, 8) == std::vector<std::uint8_t>(8, 0xa7),
            "Trace output changed adjacent caller bytes");
    session.machine.CheckAccess(mask, 8, rw); session.machine.CheckAccess(table, 65 * 8, rw);
    session.store(mask, 0x8000000000000001ULL); session.store(table, 0x1122334455667788ULL);
    session.store(table + 64 * 8, 0x8877665544332211ULL);
    require(session.object(guard) == guardValue && session.object(program) == nameAddress,
            "Writing the last supported trace slot corrupted neighboring imported objects");
    (void)session.invoke(gate, 0x3008);
    require(session.qword(0x3018) == mask && session.qword(0x3020) == table && session.qword(mask) == 0x8000000000000001ULL &&
            session.qword(table) == 0x1122334455667788ULL && session.qword(table + 64 * 8) == 0x8877665544332211ULL,
            "Repeated trace call reset persistent state or changed returned guest pointers");
    const std::uint64_t wrongSize = 31;
    session.machine.Write(0x3008, std::as_bytes(std::span(&wrongSize, 1)));
    const auto unsupported = session.bytes(0x3000, 48);
    rejects([&] { (void)session.invoke(gate, 0x3008); }, "Unsupported SCE libc bootstrap trace info size:");
    require(session.bytes(0x3000, 48) == unsupported, "Unsupported trace layout partially changed caller bytes");
    session.machine.Write(0x3008, std::as_bytes(std::span(input)));
    session.machine.Write(0x6ff0, std::as_bytes(std::span(input).first(2)));
    const auto boundary = session.bytes(0x6ff0, 16);
    const auto unchanged = session.bytes(0x3000, 48);
    session.machine.Protect(0x3000, 4096, Permission::Read);
    for (const auto address : std::array<std::uint64_t, 4>{0, 0x3008, 0x6ff0, std::numeric_limits<std::uint64_t>::max() - 1}) {
        rejects([&] { (void)session.invoke(gate, address); }, address == 0 || address > 0x9000 ? "guest span is invalid" : "Guest access denied");
        require(session.bytes(0x3000, 48) == unchanged, "Rejected trace request partially changed another output buffer");
        require(session.bytes(0x6ff0, 16) == boundary, "Rejected cross-boundary trace partially changed its writable prefix");
    }
    session.machine.Map(0x7000, 4096, Permission::Read);
    session.machine.Write(0x6ff0, std::as_bytes(std::span(input)));
    const auto mixedPermissions = session.bytes(0x6ff0, 32);
    rejects([&] { (void)session.invoke(gate, 0x6ff0); }, "Guest access denied");
    require(session.bytes(0x6ff0, 32) == mixedPermissions,
            "Trace output wrote before rejecting its readable-only suffix");
}

void scopeAndLifetime() {
    {
        Session session;
        const auto getter = session.resolve("959qrazPIrg");
        require(session.resolve("959qrazPIrg") == getter, "Repeated qualified resolution changed the guest gate");
        auto wrong = qualified("959qrazPIrg"); wrong.LibraryVersion = 2;
        rejects([&] { session.imports.Resolve(wrong); }, "scope/version");
        wrong = qualified("NWtTN10cJzE", true); wrong.ModuleName = "libkernel";
        rejects([&] { session.imports.Resolve(wrong); }, "scope/version");
        require(!session.imports.Resolve(qualified("AAAAAAAAAAA")), "Bootstrap resolver captured an unknown service");
        wrong = qualified("959qrazPIrg"); wrong.LibraryName = "unrelated"; wrong.ModuleName = "unrelated";
        require(!session.imports.Resolve(wrong), "Bootstrap resolver captured an unrelated provider's known NID");
    }
    Cpu::Machine machine;
    machine.Map(0x1000, 4096, rx); machine.Map(0x2000, 4096, rw); machine.Map(0x4000, 4096, rw);
    const std::array<std::uint8_t, 6> call{0xff, 0x15, 0xfa, 0x0f, 0, 0};
    machine.Write(0x1000, std::as_bytes(std::span(call)));
    std::uint64_t gate;
    { Cpu::SceLibcBootstrapImports owned(machine); gate = owned.Resolve(qualified("959qrazPIrg")).value(); }
    machine.Write(0x2000, std::as_bytes(std::span(&gate, 1))); machine.Set(Register::Rsp, 0x4ff0);
    rejects([&] { machine.Run(0x1000, 0x1006, 100); }, "runtime has expired");
}
}
int main() {
    try {
        processParameters(); callbackRegistration(); objectsAndTrace(); scopeAndLifetime();
        std::cout << "PASS actual x86 libc bootstrap gates, guest objects, callbacks, process parameters and persistent trace storage\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
