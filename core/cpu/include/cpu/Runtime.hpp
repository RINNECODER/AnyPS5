#pragma once

#include <cpu/Cpu.hpp>

namespace Cpu {

class LinuxRuntime {
public:
    explicit LinuxRuntime(Machine& machine);
private:
    static void syscall(Machine& machine);
};

}
