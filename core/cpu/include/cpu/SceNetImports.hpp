#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

// Native provider for libSceNet sockets/resolver/epoll/pools, the offline libSceHttp2 and
// libSceSsl surface, and the libkernel (libScePosix) socket calls. Guest sockaddr layouts,
// errno numbers and socket options are the PS5/FreeBSD ones and are converted explicitly;
// no guest pointer is dereferenced on the host, everything goes through Machine::Read/Write.
class SceNetImports {
public:
    explicit SceNetImports(Machine& machine, std::uint64_t gateBase = 0x7ffd80000000);
    ~SceNetImports();
    SceNetImports(const SceNetImports&) = delete;
    SceNetImports& operator=(const SceNetImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
    // Address of the calling thread's errno cell, or 0 outside a guest thread.
    void SetErrnoAddress(std::function<std::uint64_t()> provider);
    // Polled while a blocking socket call waits; true makes the call fail with EINTR.
    void SetStopCheck(std::function<bool()> check);
    // Signals a user event on a guest equeue (queue handle, event id, data); returns 0 on success.
    void SetUserEventTrigger(std::function<int(std::uint64_t, std::int32_t, std::uint64_t)> trigger);

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
