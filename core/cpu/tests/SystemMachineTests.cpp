#include <cpu/SystemMachine.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::size_t ramBytes = 16 * 1024 * 1024;
constexpr std::uint64_t firstValue = 0x1122334455667788;
constexpr std::uint64_t secondValue = 0x8877665544332211;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::byte> load(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error(std::string("Cannot read kernel fixture: ") + path);
    const auto length = input.tellg();
    require(length > 0 && length <= 32768, "Kernel fixture is empty or exceeds its reserved load range");
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    require(static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), length)), "Short kernel fixture read");
    return bytes;
}

template<class T> T physical(const Cpu::SystemMachine& machine, std::uint64_t address) {
    T value{};
    machine.ReadPhysical(address, std::as_writable_bytes(std::span(&value, 1)));
    return value;
}

void mappedResults(const Cpu::SystemMachine& machine) {
    require((machine.GetCR0() & 0x80000001) == 0x80000001, "Guest did not enable protected-mode paging");
    require((machine.GetCR4() & 0x20) == 0x20, "Guest did not enable PAE");
    require(machine.GetCR3() == 0xd000, "Guest did not switch to its second page-table root");
    require(physical<std::uint64_t>(machine, 0x30000) == firstValue, "First virtual alias write did not reach its physical page");
    require(physical<std::uint64_t>(machine, 0x31000) == secondValue, "CR3 switch reused the stale virtual alias translation");
    require(physical<std::uint64_t>(machine, 0x400000) == 0, "Virtual alias was treated as an identity physical address");
}

void kernelPaging(const char* path) {
    Cpu::SystemMachine machine(ramBytes);
    machine.WritePhysical(0x1000, load(path));
    require(machine.Run(0x1000, 0x1f0000, 1000) == Cpu::SystemStop::Halted, "Paging kernel did not execute HLT");
    mappedResults(machine);
    require(machine.GetCR2() == 0, "Successful paging kernel unexpectedly recorded a page fault");
}

void kernelPageFault(const char* path) {
    Cpu::SystemMachine machine(ramBytes);
    machine.WritePhysical(0x1000, load(path));
    try {
        machine.Run(0x1000, 0x1f0000, 1000);
    } catch (const std::exception& error) {
        const std::string diagnostic = error.what();
        require(diagnostic.find("exception vector=14") != std::string::npos, error.what());
        require(diagnostic.find("RIP=0x") != std::string::npos, "System exception did not identify the faulting instruction");
        require(diagnostic.find("CR2=0x401000") != std::string::npos, "Page fault diagnostic lost the faulting virtual address");
        require(diagnostic.find("guest IDT exception delivery is unsupported") != std::string::npos,
                "Unsupported exception delivery was hidden");
        require(machine.GetCR2() == 0x401000, "Architectural CR2 did not contain the absent virtual page");
        mappedResults(machine);
        return;
    }
    throw std::runtime_error("Absent guest PTE did not produce an explicit architectural page fault");
}

void unsupportedSystemServices() {
    struct Case { std::vector<std::uint8_t> bytes; const char* diagnostic; };
    const Case cases[] = {
        {{0x0f, 0x05}, "SYSCALL entry"},
        {{0x0f, 0x34}, "SYSENTER entry"},
        {{0xe4, 0x80}, "port input"},
        {{0xe6, 0x80}, "port output"},
        {{0xb9, 0x1b, 0, 0, 0, 0x0f, 0x32}, "MSR operation"},
        {{0xb9, 0x1b, 0, 0, 0, 0x0f, 0x30}, "MSR operation"},
        {{0x44, 0x0f, 0x20, 0xc0}, "control register/APIC operation"},
    };
    for (const auto& test : cases) {
        Cpu::SystemMachine machine(4096);
        machine.WritePhysical(0x100, std::as_bytes(std::span(test.bytes)));
        try {
            machine.Run(0x100, 0xff0, 10);
        } catch (const std::exception& error) {
            require(std::string(error.what()).find(test.diagnostic) != std::string::npos, error.what());
            continue;
        }
        throw std::runtime_error(std::string("Unsupported system service did not fail: ") + test.diagnostic);
    }
}

void rejectedControlValues() {
    struct Case { unsigned control; std::uint64_t value; const char* diagnostic; };
    constexpr std::array cases{
        Case{4, 1ULL << 40, "Unsupported system CR4 value"},
        Case{0, 1ULL << 40, "Unsupported system CR0 value"},
        Case{4, 0x50200 | (1ULL << 21), "Unsupported system CR4 value"},
        Case{4, 0x50200 | (1ULL << 22), "Unsupported system CR4 value"},
    };
    std::string failures;
    for (const auto& test : cases) {
        Cpu::SystemMachine machine(4096);
        const auto before = test.control == 0 ? machine.GetCR0() : machine.GetCR4();
        std::vector<std::uint8_t> bytes{0x48, 0xb8};
        for (unsigned shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(test.value >> shift));
        bytes.insert(bytes.end(), {0x0f, 0x22, static_cast<std::uint8_t>(0xc0 | (test.control << 3)), 0xf4});
        machine.WritePhysical(0x100, std::as_bytes(std::span(bytes)));
        bool rejected = false;
        try {
            machine.Run(0x100, 0xff0, 10);
        } catch (const std::exception& error) {
            require(std::string(error.what()).find(test.diagnostic) != std::string::npos, error.what());
            rejected = true;
        }
        const auto after = test.control == 0 ? machine.GetCR0() : machine.GetCR4();
        if (!rejected || after != before)
            failures += std::string(test.diagnostic) + " was not rejected before CPU state mutation; ";
    }
    if (!failures.empty()) throw std::runtime_error(failures);
}

void boundedExecution() {
    Cpu::SystemMachine machine(4096);
    constexpr std::array<std::uint8_t, 2> loop{0xeb, 0xfe};
    machine.WritePhysical(0x100, std::as_bytes(std::span(loop)));
    require(machine.Run(0x100, 0xff0, 7) == Cpu::SystemStop::InstructionLimit, "Ring0 loop ignored its instruction budget");
    require(machine.GetRip() == 0x100, "Ring0 instruction budget changed the loop destination");
    const auto rejected = [](const std::function<void()>& operation) {
        try { operation(); }
        catch (const std::invalid_argument&) { return; }
        throw std::runtime_error("Physical RAM boundary or instruction limit was not enforced");
    };
    std::array<std::byte, 2> buffer{};
    rejected([&] { machine.ReadPhysical(4095, buffer); });
    rejected([&] { machine.WritePhysical(std::numeric_limits<std::uint64_t>::max(), buffer); });
    rejected([&] { machine.Run(0x100, 0xff0, 0); });
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "Expected successful and faulting kernel fixture paths");
        kernelPaging(argv[1]);
        kernelPageFault(argv[2]);
        unsupportedSystemServices();
        rejectedControlValues();
        boundedExecution();
        std::cout << "PASS guest ring0 page tables, CR3 remapping, architectural page fault, explicit unsupported services, and bounded execution\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
