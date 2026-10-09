#pragma once

#include <cpu/Cpu.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace Cpu {

// Many host functions behind one Machine host gate. The modern TCG engine has a
// fixed host-gate budget (256), far below the number of imports a retail title
// links, so each entry is a 16-byte guest trampoline that loads its index into
// R11 (call-clobbered scratch under the SysV ABI) and jumps to the shared gate.
// The gate's handler dispatches on R11 and then returns to the original caller.
class SceHostTrampolines {
public:
    SceHostTrampolines(Machine& machine, std::uint64_t base, std::size_t capacity);
    ~SceHostTrampolines();
    SceHostTrampolines(const SceHostTrampolines&) = delete;
    SceHostTrampolines& operator=(const SceHostTrampolines&) = delete;
    // Returns the guest address of a new trampoline that runs handler when called.
    std::uint64_t Add(std::function<void(Machine&)> handler);
    std::size_t Size() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
