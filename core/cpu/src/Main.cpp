#include <cpu/Cpu.hpp>
#include <cpu/ElfLoader.hpp>
#include <cpu/Runtime.hpp>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::runtime_error("Usage: anyps5_cpu_run <static-x86-64.elf> [guest arguments...]");
        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
            throw std::runtime_error("Cannot configure standalone CLI SIGPIPE handling");
        Cpu::Machine machine;
        Cpu::LinuxRuntime runtime(machine);
        auto image = Cpu::Load(machine, argv[1]);
        std::vector<std::string> arguments;
        for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
        Cpu::SetupStack(machine, image, arguments);
        std::cerr << "host=arm64 backend=" << Cpu::Machine::Backend() << " guest=x86_64 entry=0x" << std::hex << image.Entry << std::dec << '\n';
        const auto reason = machine.Run(image.Entry, 0, 100000000);
        if (reason != Cpu::StopReason::Exit) throw std::runtime_error("Guest did not exit: CPU execution budget or stop reached");
        std::cerr << "guest_exit=" << machine.ExitCode() << '\n';
        return machine.ExitCode();
    } catch (const std::exception& error) {
        std::cerr << "anyps5_cpu_run: " << error.what() << '\n';
        return 1;
    }
}
