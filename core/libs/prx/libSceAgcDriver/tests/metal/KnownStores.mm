#include "MetalTestSupport.hpp"
#include "MetalTimeline.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

// MetalTimeline::PendingBytes: what pending GPU work leaves in guest memory when its stores are known
// before it runs (a guest fill kernel clearing DCC keys, keys a draw leaves uncompressed), so the
// driver need not wait for that work to read them; and when it must wait after all.

namespace {

using AgcDriver::Metal::MetalTimeline;
using MetalTests::Require;

constexpr std::uint64_t Keys = 0x400000;
constexpr std::size_t Count = 64;

MetalTimeline::KnownStore Store(std::byte value, std::vector<std::byte> before, std::uint64_t begin = Keys, std::size_t count = Count) {
    MetalTimeline::KnownStore store{begin, begin + count, {}, std::move(before)};
    store.pattern.fill(value);
    return store;
}

bool All(std::span<const std::byte> bytes, std::byte value) {
    return std::all_of(bytes.begin(), bytes.end(), [&](std::byte b) { return b == value; });
}

}

void RunKnownStoresTests(const MetalTests::Context& context) {
    id<MTLCommandQueue> queue = [context.device newCommandQueue];
    const auto commit = [&](MetalTimeline& timeline, std::vector<std::pair<std::uint64_t, std::uint64_t>> written,
                             std::vector<MetalTimeline::KnownStore> known = {}, bool unknown = false) {
        timeline.Commit([queue commandBuffer], {}, unknown, std::move(written), std::move(known));
    };
    const std::vector<std::byte> stale(Count, std::byte{0xff});
    std::vector<std::byte> out(Count);

    // A pending fill of clear keys (0x00) over uncompressed keys: known whether or not it has landed.
    {
        MetalTimeline timeline;
        commit(timeline, {{Keys, Keys + Count}}, {Store(std::byte{0x00}, stale)});
        Require(timeline.MayHaveWritten(Keys, Keys + Count), "a pending fill did not mark its keys written");
        Require(timeline.PendingBytes(Keys, stale, out) && All(out, std::byte{0x00}), "keys a pending fill clears were not known before it landed");
        std::vector<std::byte> landed(Count, std::byte{0x00});
        std::fill_n(landed.begin(), Count / 2, std::byte{0xff});
        Require(timeline.PendingBytes(Keys, landed, out) && All(out, std::byte{0x00}), "keys a pending fill clears were not known while it lands");
        // A guest CPU write the fill does not explain: the keys are not known, the driver waits.
        auto written = stale;
        written[7] = std::byte{0x40};
        Require(!timeline.PendingBytes(Keys, written, out), "keys the guest CPU rewrote were taken from a pending fill");
        // Part of the range, or a range past the fill: not known.
        Require(timeline.PendingBytes(Keys + 16, std::span(stale).first(16), std::span(out).first(16)), "a range inside the fill was not known");
        Require(!timeline.PendingBytes(Keys + 32, stale, out), "a range past the fill was taken from it");
        timeline.Drain();
        Require(!timeline.MayHaveWritten(Keys, Keys + Count) && !timeline.PendingBytes(Keys, stale, out), "a drained fill stayed pending");
    }
    // Later work that stores to the keys without a known pattern hides the fill; unknown (BDA) writes too.
    {
        MetalTimeline timeline;
        commit(timeline, {{Keys, Keys + Count}}, {Store(std::byte{0x00}, stale)});
        commit(timeline, {{Keys + 8, Keys + 12}});
        Require(!timeline.PendingBytes(Keys, stale, out), "keys later GPU work stores to were taken from an earlier fill");
        timeline.Drain();
        commit(timeline, {{Keys, Keys + Count}}, {Store(std::byte{0x00}, stale)}, true);
        Require(!timeline.PendingBytes(Keys, stale, out), "keys were taken from a fill with unknown writes pending");
        timeline.Drain();
    }
    // A draw that writes the image after the fill leaves the keys uncompressed (0xff) once it completes:
    // later work knows that, whichever of the two stores the CPU sees landed.
    {
        MetalTimeline timeline;
        std::vector<std::byte> original(Count, std::byte{0x93});
        commit(timeline, {{Keys, Keys + Count}}, {Store(std::byte{0x40}, original)});
        commit(timeline, {{Keys, Keys + Count}, {0x800000, 0x900000}}, {Store(std::byte{0xff}, original)});
        for (const std::byte visible : {std::byte{0x93}, std::byte{0x40}, std::byte{0xff}}) {
            const std::vector<std::byte> seen(Count, visible);
            Require(timeline.PendingBytes(Keys, seen, out) && All(out, std::byte{0xff}), "keys a pending draw leaves uncompressed were not known");
        }
        const std::vector<std::byte> foreign(Count, std::byte{0x80});
        Require(!timeline.PendingBytes(Keys, foreign, out), "keys no pending store explains were taken from one");
        timeline.Drain();
        Require(!timeline.MayHaveWritten(Keys, Keys + Count), "drained work still marked the keys written");
    }
    // Reaping retires a completed work's ranges while later work is still pending.
    {
        MetalTimeline timeline;
        id<MTLCommandBuffer> first = [queue commandBuffer];
        timeline.Commit(first, {}, false, {{Keys, Keys + Count}}, {Store(std::byte{0x00}, stale)});
        [first waitUntilCompleted];
        id<MTLCommandBuffer> blocked = [queue commandBuffer];
        id<MTLSharedEvent> event = [context.device newSharedEvent];
        [blocked encodeWaitForEvent:event value:1];
        timeline.Commit(blocked, {}, false, {{0x800000, 0x900000}});
        Require(timeline.Reap(), "a completed work was not reaped");
        Require(!timeline.MayHaveWritten(Keys, Keys + Count) && !timeline.PendingBytes(Keys, stale, out),
                "a reaped work's stores stayed pending behind later work");
        Require(timeline.MayHaveWritten(0x800000, 0x800001), "a pending work's range was retired early");
        event.signaledValue = 1;
        timeline.Drain();
    }
}
