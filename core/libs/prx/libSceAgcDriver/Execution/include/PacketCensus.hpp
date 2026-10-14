#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PACKETCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PACKETCENSUS_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4Opcodes.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Opt-in census of what the guest submits (off by default). ANYPS5_AGC_PACKET_CENSUS=1 walks every
// executed submission once, before its packets run, and times each packet the worker executes.
// Every 32 flips one JSON line goes to stderr:
//   {"event":"agc_packet_census","flips":32,...}
// with per-window totals: submissions, packets, dwords, draws, dispatches, a per-packet-kind
// histogram (count, dwords, worker ms), how many submissions / inlined INDIRECT_BUFFER targets /
// draw segments / 128-dword chunks are byte-identical to one seen in the previous 1 and 8 flips,
// how close each submission is to a same-length one of the previous flip, and how many register
// writes set a register to the value it already had. It measures only: nothing it computes feeds
// execution.
namespace AgcDriver::PacketCensus {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ANYPS5_AGC_PACKET_CENSUS");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return enabled;
}

// Packet kinds: PM4 opcodes 0..255, then the AGC driver's own NOP-encoded packets and tag markers.
inline constexpr std::size_t KindFlip = 256, KindRenderingWait = 257, KindFiller = 258, KindTag = 259, KindCount = 260;

inline std::size_t KindOf(std::span<const std::uint32_t> packet) {
    const auto header = packet[0];
    if (Pm4::FillerPacket(header)) return KindFiller;
    if (header == FlipPacketHeader) return KindFlip;
    if (header == RenderingWaitPacketHeader) return KindRenderingWait;
    const auto opcode = (header >> 8u) & 0xffu;
    if (opcode == 0x79u && (header & 1u) != 0 && packet.size() >= 3 && packet[1] == 0x342u) return KindTag;
    return opcode;
}

inline std::string KindName(std::size_t kind) {
    switch (kind) {
        case KindFlip: return "AGC_FLIP";
        case KindRenderingWait: return "AGC_RENDERING_WAIT";
        case KindFiller: return "FILLER";
        case KindTag: return "AGC_TAG";
        default: break;
    }
    for (const auto& opcode : Pm4::Opcodes) if (opcode.value == kind) return std::string(opcode.name);
    char name[16];
    std::snprintf(name, sizeof name, "IT_0x%02zx", kind);
    return name;
}

inline std::uint64_t Hash(std::span<const std::uint32_t> words) {
    std::uint64_t h = 0x9e3779b97f4a7c15ull ^ (words.size() * 0xff51afd7ed558ccdull);
    for (const auto word : words) {
        h ^= word;
        h *= 0x100000001b3ull;
        h ^= h >> 29u;
    }
    h ^= h >> 33u;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33u;
    return h;
}

// Items (submissions, IB targets, segments, chunks) whose hash was seen in the previous 1 / 8 flips.
struct Repeats {
    std::uint64_t items = 0, dwords = 0, n1Items = 0, n1Dwords = 0, n8Items = 0, n8Dwords = 0;
};

class Recency {
public:
    // Tallies one item of the current flip against the hashes the earlier flips committed.
    void Note(std::uint64_t hash, std::uint64_t dwords, std::uint64_t flip, Repeats& out) {
        ++out.items;
        out.dwords += dwords;
        if (const auto found = seen.find(hash); found != seen.end()) {
            const auto age = flip - found->second;
            if (age <= 1) { ++out.n1Items; out.n1Dwords += dwords; }
            if (age <= 8) { ++out.n8Items; out.n8Dwords += dwords; }
        }
        pending.push_back(hash);
    }
    void Commit(std::uint64_t flip) {
        for (const auto hash : pending) seen.insert_or_assign(hash, flip);
        pending.clear();
        if (flip % 32 == 0) std::erase_if(seen, [&](const auto& entry) { return flip - entry.second > 8; });
    }

private:
    std::unordered_map<std::uint64_t, std::uint64_t> seen;
    std::vector<std::uint64_t> pending;
};

struct KindTotals {
    std::uint64_t count = 0, dwords = 0;
};

struct RegisterTotals {
    std::uint64_t writes = 0, redundant = 0;
};

struct State {
    std::mutex mutex;
    std::uint64_t flip = 1;
    std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    // Window totals.
    std::uint64_t submissions = 0, packets = 0, dwords = 0, draws = 0, dispatches = 0;
    std::uint64_t indirectPackets = 0, indirectDwords = 0;
    std::array<KindTotals, KindCount> kinds{};
    Repeats submissionRepeats, indirectRepeats, segmentRepeats, shapeRepeats, chunkRepeats;
    // Closest same-length submission of the previous 8 flips: identical, <=16, <=256, more dwords differ, none.
    std::array<std::uint64_t, 5> nearItems{}, nearDwords{};
    // SET_CONTEXT_REG, SET_SH_REG, SET_UCONFIG_REG(_INDEX), the *_INDIRECT lists.
    std::array<RegisterTotals, 4> registers{};
    Recency submissionSeen, indirectSeen, segmentSeen, shapeSeen, chunkSeen;
    // The last 8 flips' submissions (newest first) and the current flip's, per (queue, length),
    // for the near-identical diff.
    std::deque<std::unordered_multimap<std::uint64_t, std::vector<std::uint32_t>>> previousFlips;
    std::unordered_multimap<std::uint64_t, std::vector<std::uint32_t>> currentFlip;
};

inline State& Shared() {
    static State state;
    return state;
}

// Worker time per packet kind, outside the mutex.
inline std::array<std::atomic<std::uint64_t>, KindCount>& KindNanoseconds() {
    static std::array<std::atomic<std::uint64_t>, KindCount> ns{};
    return ns;
}

// Called on the worker just before a register packet executes (so predicated-off and COND_EXEC-skipped
// packets are not counted): a write is redundant when the queue already holds that value, defaults
// after CLEAR_STATE included.
inline void NoteRegisterWrites(std::span<const std::uint32_t> packet, const std::vector<std::uint32_t>* pairs, const QueueState& queue) {
    const auto header = packet[0];
    if ((header >> 30u) != 3u || packet.size() < 3) return;
    const auto opcode = (header >> 8u) & 0xffu;
    const bool indirect = Pm4::IndirectRegisterOpcode(opcode);
    if (!indirect && opcode != 0x69u && opcode != 0x76u && opcode != 0x79u && opcode != 0x7au) return;
    if (KindOf(packet) == KindTag) return;
    const auto& registers = (opcode == 0x69u || opcode == 0x9fu) ? queue.context : (opcode == 0x76u || opcode == 0x63u) ? queue.shader : queue.userConfig;
    const std::size_t group = indirect ? 3 : opcode == 0x69u ? 0 : opcode == 0x76u ? 1 : 2;
    std::uint64_t writes = 0, redundant = 0;
    const auto note = [&](std::uint32_t offset, std::uint32_t value) {
        ++writes;
        if (const auto found = registers.find(offset); found != registers.end() && (*found).second == value) ++redundant;
    };
    if (indirect) {
        if (pairs == nullptr) return;
        for (std::size_t i = 0; i + 1 < pairs->size(); i += 2) note((*pairs)[i] & ~0x70000000u, (*pairs)[i + 1]);
    } else {
        const auto offset = packet[1] & ~0x70000000u;
        if (offset > 0xffffu) return;
        for (std::size_t i = 2; i < packet.size(); ++i) note(offset + static_cast<std::uint32_t>(i - 2), packet[i]);
    }
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    state.registers[group].writes += writes;
    state.registers[group].redundant += redundant;
}

// Walks a submission before it executes. The captured words already have INDIRECT_BUFFER targets
// inlined; Submission::censusIndirect lists where each one landed.
inline void NoteSubmission(const DriverDetail::Submission& submission) {
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    const auto words = std::span<const std::uint32_t>(submission.commands);
    const auto flip = state.flip;
    ++state.submissions;
    state.dwords += words.size();
    state.submissionSeen.Note(Hash(words), words.size(), flip, state.submissionRepeats);

    // Near-identical: the fewest differing dwords against a same-length submission of the previous 8 flips.
    const auto sizeKey = (static_cast<std::uint64_t>(submission.queue) << 40u) | words.size();
    std::size_t bucket = 4;
    std::size_t best = words.size() + 1;
    int compared = 0;
    for (const auto& flipSubmissions : state.previousFlips) {
        const auto [first, last] = flipSubmissions.equal_range(sizeKey);
        for (auto candidate = first; candidate != last && compared < 16 && best != 0; ++candidate, ++compared) {
            std::size_t differ = 0;
            for (std::size_t i = 0; i < words.size() && differ < best; ++i) differ += candidate->second[i] != words[i];
            best = std::min(best, differ);
        }
    }
    if (compared != 0) bucket = best == 0 ? 0 : best <= 16 ? 1 : best <= 256 ? 2 : 3;
    ++state.nearItems[bucket];
    state.nearDwords[bucket] += words.size();
    state.currentFlip.emplace(sizeKey, submission.commands);

    for (const auto& indirect : submission.censusIndirect) {
        const auto begin = static_cast<std::size_t>(indirect[1]), end = static_cast<std::size_t>(indirect[2]);
        if (begin > end || end > words.size()) continue;
        ++state.indirectPackets;
        state.indirectDwords += end - begin;
        state.indirectSeen.Note(Hash(words.subspan(begin, end - begin)), end - begin, flip, state.indirectRepeats);
    }
    for (std::size_t chunk = 0; chunk + 128 <= words.size(); chunk += 128) {
        state.chunkSeen.Note(Hash(words.subspan(chunk, 128)), 128, flip, state.chunkRepeats);
    }

    // A segment's shape hashes its packet headers and register offsets but not the values they carry,
    // so a segment that only patches constants, addresses or draw sizes keeps its shape.
    std::size_t segmentStart = 0;
    std::uint64_t shape = 0x84222325cbf29ce4ull;
    for (std::size_t cursor = 0; cursor < words.size();) {
        const auto header = words[cursor];
        const auto count = std::min(Pm4::PacketWords(header), words.size() - cursor);
        const auto packet = words.subspan(cursor, count);
        const auto kind = KindOf(packet);
        ++state.packets;
        ++state.kinds[kind].count;
        state.kinds[kind].dwords += count;
        const auto opcode = (header >> 8u) & 0xffu;
        const bool type3 = (header >> 30u) == 3u;
        shape = (shape ^ header) * 0x100000001b3ull;
        if (type3 && count >= 2 && (opcode == 0x69u || opcode == 0x76u || opcode == 0x79u || opcode == 0x7au)) shape = (shape ^ packet[1]) * 0x100000001b3ull;
        cursor += count;
        const bool draw = type3 && Pm4::DrawOpcode(opcode);
        const bool dispatch = type3 && (opcode == 0x15u || opcode == 0x16u);
        state.draws += draw;
        state.dispatches += dispatch;
        // A draw segment is the packets since the previous draw or dispatch, up to and including this one.
        if (draw || dispatch) {
            state.segmentSeen.Note(Hash(words.subspan(segmentStart, cursor - segmentStart)), cursor - segmentStart, flip, state.segmentRepeats);
            state.shapeSeen.Note(shape ^ (shape >> 31u), cursor - segmentStart, flip, state.shapeRepeats);
            segmentStart = cursor;
            shape = 0x84222325cbf29ce4ull;
        }
    }
}

inline void AppendRepeats(std::string& out, const char* name, const Repeats& r) {
    char item[192];
    std::snprintf(item, sizeof item, "\"%s\":[%llu,%llu,%llu,%llu,%llu,%llu]", name, static_cast<unsigned long long>(r.items),
        static_cast<unsigned long long>(r.dwords), static_cast<unsigned long long>(r.n1Items), static_cast<unsigned long long>(r.n1Dwords),
        static_cast<unsigned long long>(r.n8Items), static_cast<unsigned long long>(r.n8Dwords));
    out += item;
}

// Called on the worker when it executes a flip packet; every 32 flips prints one line and resets.
inline void Flip() {
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    const auto flip = state.flip;
    state.submissionSeen.Commit(flip);
    state.indirectSeen.Commit(flip);
    state.segmentSeen.Commit(flip);
    state.shapeSeen.Commit(flip);
    state.chunkSeen.Commit(flip);
    state.previousFlips.push_front(std::move(state.currentFlip));
    if (state.previousFlips.size() > 8) state.previousFlips.pop_back();
    state.currentFlip.clear();
    ++state.flip;
    if (flip % 32 != 0) return;
    const auto now = std::chrono::steady_clock::now();
    const auto windowMs = std::chrono::duration<double, std::milli>(now - state.windowStart).count();
    std::array<double, KindCount> ms{};
    double workerMs = 0;
    for (std::size_t kind = 0; kind < KindCount; ++kind) {
        ms[kind] = static_cast<double>(KindNanoseconds()[kind].exchange(0, std::memory_order_relaxed)) / 1e6;
        workerMs += ms[kind];
    }
    std::vector<std::size_t> order;
    for (std::size_t kind = 0; kind < KindCount; ++kind) if (state.kinds[kind].count != 0 || ms[kind] != 0) order.push_back(kind);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return ms[a] != ms[b] ? ms[a] > ms[b] : state.kinds[a].count > state.kinds[b].count; });
    std::string line;
    char item[256];
    std::snprintf(item, sizeof item,
        "{\"schema_version\":1,\"event\":\"agc_packet_census\",\"flip\":%llu,\"flips\":32,\"window_ms\":%.1f,\"worker_ms\":%.1f,"
        "\"submissions\":%llu,\"packets\":%llu,\"dwords\":%llu,\"draws\":%llu,\"dispatches\":%llu,\"ib\":[%llu,%llu],",
        static_cast<unsigned long long>(flip), windowMs, workerMs, static_cast<unsigned long long>(state.submissions),
        static_cast<unsigned long long>(state.packets), static_cast<unsigned long long>(state.dwords), static_cast<unsigned long long>(state.draws),
        static_cast<unsigned long long>(state.dispatches), static_cast<unsigned long long>(state.indirectPackets),
        static_cast<unsigned long long>(state.indirectDwords));
    line += item;
    line += "\"repeat\":{";
    AppendRepeats(line, "submission", state.submissionRepeats);
    line += ',';
    AppendRepeats(line, "ib_target", state.indirectRepeats);
    line += ',';
    AppendRepeats(line, "draw_segment", state.segmentRepeats);
    line += ',';
    AppendRepeats(line, "draw_segment_shape", state.shapeRepeats);
    line += ',';
    AppendRepeats(line, "chunk128", state.chunkRepeats);
    line += "},\"near\":{\"items\":[";
    for (std::size_t i = 0; i < state.nearItems.size(); ++i) line += (i ? "," : "") + std::to_string(state.nearItems[i]);
    line += "],\"dwords\":[";
    for (std::size_t i = 0; i < state.nearDwords.size(); ++i) line += (i ? "," : "") + std::to_string(state.nearDwords[i]);
    line += "]},\"registers\":{";
    static constexpr const char* RegisterNames[] = {"context", "sh", "uconfig", "indirect"};
    for (std::size_t i = 0; i < state.registers.size(); ++i) {
        std::snprintf(item, sizeof item, "%s\"%s\":[%llu,%llu]", i ? "," : "", RegisterNames[i],
            static_cast<unsigned long long>(state.registers[i].writes), static_cast<unsigned long long>(state.registers[i].redundant));
        line += item;
    }
    line += "},\"kinds\":[";
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto kind = order[i];
        std::snprintf(item, sizeof item, "%s[\"%s\",%llu,%llu,%.2f]", i ? "," : "", KindName(kind).c_str(),
            static_cast<unsigned long long>(state.kinds[kind].count), static_cast<unsigned long long>(state.kinds[kind].dwords), ms[kind]);
        line += item;
    }
    line += "]}\n";
    std::fputs(line.c_str(), stderr);
    state.windowStart = now;
    state.submissions = state.packets = state.dwords = state.draws = state.dispatches = 0;
    state.indirectPackets = state.indirectDwords = 0;
    state.kinds = {};
    state.submissionRepeats = state.indirectRepeats = state.segmentRepeats = state.shapeRepeats = state.chunkRepeats = {};
    state.nearItems = {};
    state.nearDwords = {};
    state.registers = {};
}

// Times one executed packet on the worker, by kind; inert when the census is off.
class PacketTimer {
public:
    explicit PacketTimer(std::span<const std::uint32_t> packet)
        : kind(Enabled() ? KindOf(packet) : KindCount), start(kind != KindCount ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~PacketTimer() { Finish(); }
    // Records now (once); the flip packet finishes its timer before Flip() closes the window.
    void Finish() {
        if (kind == KindCount) return;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        KindNanoseconds()[kind].fetch_add(static_cast<std::uint64_t>(ns), std::memory_order_relaxed);
        kind = KindCount;
    }
    PacketTimer(const PacketTimer&) = delete;
    PacketTimer& operator=(const PacketTimer&) = delete;

private:
    std::size_t kind;
    std::chrono::steady_clock::time_point start;
};

}

#endif
