#pragma once

// Pure bookkeeping shared by the AGC frame capture and the replay tool (no Metal, no I/O), so the
// two rules below can be tested on their own.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace AgcDriver::Metal::AgcCapture {

// Which presents belong to the recorded frames, and when the capture is complete. Present k scans
// out flip k, so the presents of the recorded flips are target .. target + frames - 1. A present
// in that window without a buffer (a blanked output) still takes its slot: it has no Display
// record, but it counts towards completion, otherwise the End record would never be written.
class PresentTracker {
public:
    struct Step {
        bool inWindow = false;
        bool recordDisplay = false;  // the present has a buffer to record
        std::uint32_t frame = 0;
        bool complete = false;       // every frame of the window has been presented
    };

    // `armed` is false while the capture is not recording: the present is only counted.
    Step Observe(std::uint64_t target, std::uint64_t frames, bool armed, bool hasBuffer) {
        ++presents_;
        Step step;
        if (!armed || presents_ < target || presents_ >= target + frames) return step;
        step.inWindow = true;
        step.recordDisplay = hasBuffer;
        step.frame = static_cast<std::uint32_t>(presents_ - target);
        step.complete = ++seen_ == frames;
        return step;
    }
    std::uint64_t Presents() const { return presents_; }

private:
    std::uint64_t presents_ = 0;
    std::uint64_t seen_ = 0;
};

// The guest ranges of a capture over its submissions. A range recorded while the frame was running
// exists only from the submission that introduced it (`activation`, an index into the capture's
// global submission order; 0 for the pre-frame snapshot), and replaces an earlier range it overlaps.
class RangeTimeline {
public:
    void Add(std::uint64_t guest, std::uint64_t bytes, std::size_t activation) { ranges_.push_back({guest, bytes, activation}); }

    // Indices (in Add order) of the ranges that exist when `submission` runs.
    std::vector<std::size_t> LiveAt(std::size_t submission) const {
        std::vector<std::size_t> live;
        for (std::size_t i = 0; i < ranges_.size(); ++i) {
            if (ranges_[i].activation > submission) continue;
            bool replaced = false;
            for (std::size_t j = 0; j < ranges_.size() && !replaced; ++j)
                replaced = j != i && ranges_[j].activation <= submission && Replaces(j, i);
            if (!replaced) live.push_back(i);
        }
        return live;
    }

    // The first submission after `submission` at which the set of live ranges changes, or npos.
    std::size_t NextChange(std::size_t submission) const {
        std::size_t next = npos;
        for (const auto& range : ranges_)
            if (range.activation > submission) next = std::min(next, range.activation);
        return next;
    }

    static constexpr std::size_t npos = ~std::size_t{0};

private:
    struct Entry {
        std::uint64_t guest;
        std::uint64_t bytes;
        std::size_t activation;
    };
    // Whether range j (activated no later than the submission asked about) takes the place of range i.
    bool Replaces(std::size_t j, std::size_t i) const {
        const auto& a = ranges_[j];
        const auto& b = ranges_[i];
        const bool overlap = a.guest < b.guest + b.bytes && b.guest < a.guest + a.bytes;
        return overlap && (a.activation > b.activation || (a.activation == b.activation && j > i));
    }
    std::vector<Entry> ranges_;
};

}
