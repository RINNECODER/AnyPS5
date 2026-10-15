#include "MetalTestSupport.hpp"
#include "prx/libSceAgcDriver/Execution/Metal/AgcCaptureLogic.hpp"

// The capture's present bookkeeping and the replay's range timeline.

namespace {

using AgcDriver::Metal::AgcCapture::PresentTracker;
using AgcDriver::Metal::AgcCapture::RangeTimeline;
using MetalTests::Require;

void PresentWithoutBufferStillCompletes() {
    PresentTracker tracker;
    // Presents 1..2 precede the window; the window is presents 3..5 and the middle one is blank.
    Require(!tracker.Observe(3, 3, true, true).inWindow, "present before the window was taken");
    Require(!tracker.Observe(3, 3, true, true).inWindow, "present before the window was taken");
    const auto first = tracker.Observe(3, 3, true, true);
    Require(first.inWindow && first.recordDisplay && first.frame == 0 && !first.complete, "first frame of the window");
    const auto blank = tracker.Observe(3, 3, true, false);
    Require(blank.inWindow && !blank.recordDisplay && blank.frame == 1 && !blank.complete, "a blank present must keep its slot without a Display");
    const auto last = tracker.Observe(3, 3, true, true);
    Require(last.inWindow && last.recordDisplay && last.frame == 2 && last.complete, "capture with a blank present never completed");
    Require(!tracker.Observe(3, 3, true, true).inWindow, "present after the window was taken");
}

void LateRangeExistsOnlyFromItsSubmission() {
    RangeTimeline timeline;
    timeline.Add(0x1000, 0x1000, 0);   // pre-frame
    timeline.Add(0x9000, 0x1000, 3);   // introduced by submission 3
    timeline.Add(0x20000, 0x1000, 5);  // introduced by submission 5
    Require(timeline.LiveAt(0) == std::vector<std::size_t>{0}, "a range mapped mid-frame exists before its submission");
    Require(timeline.LiveAt(2) == std::vector<std::size_t>{0}, "a range mapped mid-frame exists before its submission");
    Require((timeline.LiveAt(3) == std::vector<std::size_t>{0, 1}), "a range is missing at the submission that introduced it");
    Require((timeline.LiveAt(7) == std::vector<std::size_t>{0, 1, 2}), "a range disappeared after its submission");
    Require(timeline.NextChange(0) == 3 && timeline.NextChange(3) == 5 && timeline.NextChange(5) == RangeTimeline::npos, "next change");
}

void ReusedAddressReplacesTheEarlierRange() {
    RangeTimeline timeline;
    timeline.Add(0x1000, 0x2000, 0);
    timeline.Add(0x2000, 0x2000, 4);  // overlaps the first from submission 4
    Require(timeline.LiveAt(3) == std::vector<std::size_t>{0}, "replacement applied early");
    Require(timeline.LiveAt(4) == std::vector<std::size_t>{1}, "the old range survived its replacement");
}

}

void RunCaptureLogicTests(const MetalTests::Context&) {
    PresentWithoutBufferStillCompletes();
    LateRangeExistsOnlyFromItsSubmission();
    ReusedAddressReplacesTheEarlierRange();
}
