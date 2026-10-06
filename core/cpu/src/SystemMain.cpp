#include <cpu/SystemMachine.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: anyps5_system_probe <flat-x86-64-kernel-at-0x1000.bin>");
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Cannot open kernel probe image");
        const auto size = file.tellg();
        if (size <= 0 || size > 1024 * 1024) throw std::runtime_error("Kernel probe image must contain 1 byte to 1 MiB");
        std::vector<std::byte> image(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(image.data()), size)) throw std::runtime_error("Cannot read complete kernel probe image");
        Cpu::SystemMachine machine(16 * 1024 * 1024);
        machine.WritePhysical(0x1000, image);
        const auto result = machine.Run(0x1000, 0x1f0000, 1000000);
        if (result != Cpu::SystemStop::Halted) throw std::runtime_error("System probe exhausted its instruction budget");
        std::cout << "System CPU probe halted: host=arm64 guest=x86_64 CR0=0x" << std::hex << machine.GetCR0()
                  << " CR3=0x" << machine.GetCR3() << " CR4=0x" << machine.GetCR4() << " RIP=0x" << machine.GetRip() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "anyps5_system_probe: " << error.what() << '\n';
        return 1;
    }
}
