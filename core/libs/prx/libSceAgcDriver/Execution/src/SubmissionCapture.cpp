#include "prx/libSceAgcDriver/Execution/include/SubmissionCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace AgcDriver {
namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

bool CaptureSegment(DriverDetail::Submission& submission, std::uint64_t guest, std::size_t words, std::size_t& budget) {
    require(words <= budget, "command buffer jumps exceed the copy limit (a jump loop?)");
    budget -= words;
    require(guest <= std::numeric_limits<std::uint64_t>::max() - words * sizeof(std::uint32_t), "command buffer address range overflow");
    if (words != 0) GuestMemory::CheckRange(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(guest)), words * sizeof(std::uint32_t), alignof(std::uint32_t));
    std::vector<std::pair<std::size_t, std::size_t>> guarded;
    const auto reach = [&](std::size_t cursor, std::size_t next) {
        std::erase_if(guarded, [&](const auto& range) {
            if (range.first != cursor) return false;
            submission.conditionalEnds.emplace(range.second, submission.commands.size());
            return true;
        });
        for (const auto& range : guarded) require(range.first >= next, "conditional execution range ends inside a packet");
    };
    std::vector<std::uint32_t> packet;
    const auto read = [&](std::size_t cursor, std::size_t count) {
        packet.resize(count);
        GuestMemory::Read(guest + cursor * sizeof(std::uint32_t), std::as_writable_bytes(std::span(packet)), alignof(std::uint32_t));
    };
    for (std::size_t cursor = 0; cursor < words;) {
        std::uint32_t header = 0;
        GuestMemory::Read(guest + cursor * sizeof(std::uint32_t), std::as_writable_bytes(std::span(&header, 1)), alignof(std::uint32_t));
        if (Pm4::FillerPacket(header)) {
            reach(cursor, cursor + 1);
            submission.commands.push_back(header);
            ++cursor;
            continue;
        }
        const auto count = (header & 0xc0000000u) == 0xc0000000u ? Pm4::PacketWords(header) : words - cursor;
        if ((header & 0xc0000000u) != 0xc0000000u || count > words - cursor) {
            read(cursor, words - cursor);
            submission.commands.insert(submission.commands.end(), packet.begin(), packet.end());
            return false;
        }
        reach(cursor, cursor + count);
        const auto opcode = (header >> 8u) & 0xffu;
        if (opcode == 0x3fu) {
            require(count == 4, "invalid INDIRECT_BUFFER size");
            read(cursor, count);
            const auto target = static_cast<std::uint64_t>(packet[1] & ~3u) | (static_cast<std::uint64_t>(packet[2] & 0xffffu) << 32u);
            const std::size_t targetWords = packet[3] & 0xfffffu;
            const bool chain = (packet[3] & (1u << 20u)) != 0;
            require(!chain || guarded.empty(), "a chained INDIRECT_BUFFER inside a conditional execution range is not implemented");
            GuestMemory::CheckRange(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(target)), targetWords * sizeof(std::uint32_t), alignof(std::uint32_t));
            if (Pm4::Predicated(header)) {
                require(!chain, "predicated command buffer chains are not implemented");
                const auto start = submission.commands.size();
                submission.commands.insert(submission.commands.end(), packet.begin(), packet.end());
                require(!CaptureSegment(submission, target, targetWords, budget), "a REWIND inside a predicated command buffer is not implemented");
                for (auto inner = start + count; inner < submission.commands.size(); inner += Pm4::PacketWords(submission.commands[inner])) {
                    const auto innerHeader = submission.commands[inner];
                    require(innerHeader != FlipPacketHeader && innerHeader != RenderingWaitPacketHeader, "flips and rendering waits in predicated command buffers are not implemented");
                }
                submission.conditionalEnds.emplace(start, submission.commands.size());
                cursor += count;
                continue;
            }
            if (CaptureSegment(submission, target, targetWords, budget)) {
                require(guarded.empty(), "a REWIND inside a conditional execution range is not implemented");
                return true;
            }
            if (chain) return false;
            cursor += count;
            continue;
        }
        read(cursor, count);
        if (opcode == 0x22u && count == 5) guarded.emplace_back(cursor + count + Pm4::ConditionalWords(packet), submission.commands.size());
        submission.commands.insert(submission.commands.end(), packet.begin(), packet.end());
        cursor += count;
        if (opcode == 0x59u) {
            require(guarded.empty(), "a REWIND inside a conditional execution range is not implemented");
            submission.rewindTail = reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(guest + cursor * sizeof(std::uint32_t)));
            submission.rewindWords = words - cursor;
            return true;
        }
    }
    reach(words, words);
    require(guarded.empty(), "conditional execution range exceeds its command buffer");
    return false;
}

}

void CaptureSubmissionCommands(DriverDetail::Submission& submission, std::uint64_t guestAddress, std::size_t words) {
    submission.commands.clear();
    submission.conditionalEnds.clear();
    std::size_t budget = std::size_t{1} << 26u;
    CaptureSegment(submission, guestAddress, words, budget);
}

}
