#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace Cpu {

struct SceImport;
class GuestThreads;


class SceThreadImports {
public:
    SceThreadImports(Machine& machine, std::shared_ptr<GuestThreads> threads,
                     std::uint64_t gateBase = 0x7ffdf1000000);
    ~SceThreadImports();
    SceThreadImports(const SceThreadImports&) = delete;
    SceThreadImports& operator=(const SceThreadImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import, std::uint8_t symbolType = 2);
    // Explicit public-source engineering ABI component. The ordinary thread
    // resolver does not admit these exports. Production target activation uses
    // the scope-checked route below.
    std::optional<std::uint64_t> ResolvePriority(const SceImport&, std::uint8_t symbolType);
    // libkernel1/module1.1/STT_FUNC rows for any importing image. FIFO1 and
    // priorities256..767 and deterministic logical RR3 are supported; OTHER2
    // fails explicitly. This does not certify the vendor real-time quantum.
    std::optional<std::uint64_t> ResolveTargetPriority(const SceImport&, std::uint8_t symbolType);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
