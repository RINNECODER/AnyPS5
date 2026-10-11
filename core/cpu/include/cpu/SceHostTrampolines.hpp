#pragma once

#include <cpu/Cpu.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Cpu {

// Many host functions behind one Machine host gate. The modern TCG engine has a
// fixed host-gate budget (256), far below the number of imports a retail title
// links, so each entry is a 16-byte guest trampoline that loads its index into
// R11 (call-clobbered scratch under the SysV ABI) and jumps to the shared gate.
// The gate's handler dispatches on R11 and then returns to the original caller.
//
// The table reserves the shared gate plus capacity slots of guest address space
// from base. Only the first page is mapped up front; the table doubles its mapped
// size as entries are added, so it never needs a second gate and uses at most a
// handful of guest mappings.
class SceHostTrampolines {
public:
    // 65535 entries reserve exactly 1 MiB of guest address space.
    static constexpr std::size_t DefaultCapacity = 65535;
    SceHostTrampolines(Machine& machine, std::uint64_t base, std::size_t capacity = DefaultCapacity,
                       std::string name = "SCE host");
    ~SceHostTrampolines();
    SceHostTrampolines(const SceHostTrampolines&) = delete;
    SceHostTrampolines& operator=(const SceHostTrampolines&) = delete;
    // Returns the guest address of a new trampoline that runs handler when called.
    // Throws HostCapacityError naming the table when it cannot hold another entry.
    std::uint64_t Add(std::function<void(Machine&)> handler);
    // Unmaps the table, which also removes its gate, so its range can be reused. The
    // destructor keeps the table mapped: a call into an expired provider still reaches
    // that provider's handler and reports the expiry.
    void Release();
    std::size_t Size() const;
    std::size_t Capacity() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
