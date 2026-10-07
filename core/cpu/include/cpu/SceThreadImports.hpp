#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace Cpu {

struct SceImport;
class GuestThreads;

struct ThreadPriorityConsumer {
    std::string_view Name;
    std::string_view Sha256;
};

class SceThreadImports {
public:
    SceThreadImports(Machine& machine, std::shared_ptr<GuestThreads> threads,
                     std::uint64_t gateBase = 0x7ffdf1000000);
    ~SceThreadImports();
    SceThreadImports(const SceThreadImports&) = delete;
    SceThreadImports& operator=(const SceThreadImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import, std::uint8_t symbolType = 2);
    // Explicit public-source engineering ABI component. The ordinary thread
    // resolver does not admit these exports. Production target activation must
    // use the source-bound route below, after hashing each actual consumer.
    std::optional<std::uint64_t> ResolvePriority(const SceImport&, std::uint8_t symbolType);
    // Observed PPSA04203 libkernel1/module1.1/STT_FUNC rows only. FIFO1 and
    // priorities256..767 are supported; OTHER/RR policies fail explicitly.
    std::optional<std::uint64_t> ResolveTargetPriority(const SceImport&, std::uint8_t symbolType,
                                                       ThreadPriorityConsumer);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
