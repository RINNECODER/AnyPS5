#include "prx/libSceAgcDriver/Execution/include/ComputeDispatch.hpp"
#include <algorithm>
#include <stdexcept>

namespace AgcDriver {
namespace {

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset) {
    const auto found = registers.find(offset);
    if (found == registers.end()) throw std::runtime_error("AGC driver: required shader register has not been written");
    return found->second;
}

}

ComputeDispatchState DecodeComputeDispatch(const QueueState& queue, std::span<const std::uint32_t> packet,
                                         void (*noteComputeRegisterRead)(std::uint32_t)) {
    if (packet.size() != 5 || (packet[0] & ~1u) != 0xc0031500u) throw std::invalid_argument("Compute dispatch requires a five-dword DISPATCH_DIRECT packet");
    if ((packet[4] & ~0xa020u) != 0x41u) throw std::invalid_argument("Compute dispatch modifiers are not implemented");
    ComputeDispatchState result{};
    result.programAddress = (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20c)) << 8u) |
                            (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20d) & 0xffu) << 40u);
    const auto userCount = (readRegister(queue.shader, 0x213) >> 1u) & 0x1fu;
    for (std::uint32_t i = 0; i < userCount; ++i) {
        const auto user = queue.shader.find(0x240 + i);
        result.userData.push_back(user == queue.shader.end() ? 0u : user->second);
    }
    result.compute = Graphics::DecodeComputeStageInfo(queue.shader, noteComputeRegisterRead);
    result.waveSize = (packet[4] & 0x8000u) != 0 ? 32u : 64u;
    result.groups = {packet[1], packet[2], packet[3]};
    if ((packet[4] & 0x20u) != 0) {
        const auto threads = result.groups;
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            if (threads[axis] % result.compute.numThreads[axis] != 0) result.compute.partialThreads = threads;
            const auto local = std::max(readRegister(queue.shader, 0x207 + axis) & 0xffffu, 1u);
            result.groups[axis] = static_cast<std::uint32_t>((static_cast<std::uint64_t>(result.groups[axis]) + local - 1) / local);
        }
    }
    return result;
}

}
