#include "prx/libSceAgcDriver/Execution/include/SubmissionValidation.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace AgcDriver {
namespace {

std::string GuestContext(std::uint64_t guestAddress) {
    if (guestAddress == 0) return {};
    char context[64];
    std::snprintf(context, sizeof(context), " (submission guest 0x%llx)", static_cast<unsigned long long>(guestAddress));
    return context;
}

}

void ValidateSubmission(const DriverDetail::Submission& submission, std::uint64_t guestAddress) {
    const std::span<const std::uint32_t> commands = submission.commands;
    const auto queue = submission.queue;
    const auto context = GuestContext(guestAddress);
    std::vector<std::size_t> guarded;
    for (std::size_t cursor = 0; cursor < commands.size();) {
        std::erase_if(guarded, [cursor](std::size_t end) { return end <= cursor; });
        const auto header = commands[cursor];
        if (Pm4::FillerPacket(header)) { ++cursor; continue; }
        if ((header & 0xc0000000u) != 0xc0000000u) {
            char what[96];
            std::snprintf(what, sizeof(what), "unsupported PM4 packet type: header 0x%08x at DWORD %zu of %zu", header, cursor, commands.size());
            throw std::runtime_error(std::string("AGC driver: ") + what + context);
        }
        const auto count = Pm4::PacketWords(header);
        if (count > commands.size() - cursor) throw std::runtime_error("AGC driver: truncated PM4 packet" + context);
        try {
            Pm4::Validate(commands.subspan(cursor, count), queue);
            if (header == FlipPacketHeader && !guarded.empty()) throw std::runtime_error("a flip inside a conditional execution range (conditional flip reservation) is not implemented");
            if (((header >> 8u) & 0xffu) == 0x22u) {
                const auto end = submission.conditionalEnds.find(cursor);
                if (end == submission.conditionalEnds.end()) throw std::runtime_error("conditional execution range is not made of whole packets of its command buffer");
                guarded.push_back(end->second);
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("AGC driver: " + Pm4::Name(header) + " at DWORD " + std::to_string(cursor) + ": " + error.what() + context);
        }
        cursor += count;
    }
}

}
