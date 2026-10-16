#pragma once

#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace AgcDriver::Metal {

using DriverDetail::Submission;

class MetalDraw;

struct DrawAheadCounters {
    std::uint64_t published = 0;
    std::uint64_t used = 0;
    std::uint64_t compiled = 0;
    std::uint64_t dropped = 0;
};

class MetalDrawAhead {
public:
    MetalDrawAhead() = default;
    ~MetalDrawAhead() = default;

    MetalDrawAhead(const MetalDrawAhead&) = delete;
    MetalDrawAhead& operator=(const MetalDrawAhead&) = delete;

    [[nodiscard]] static bool Enabled();

    void Post(const Submission& submission, const QueueState& state, MetalDraw* draw, id<MTLDevice> device);
    void Progress(std::uint64_t serial, std::size_t cursor);
    void Retire(std::uint64_t serial);

    [[nodiscard]] DrawAheadCounters TakeStatistics(bool reset = true);

private:
    std::atomic<std::uint64_t> activeSerial{0};
    std::atomic<std::uint64_t> retiredSerial{0};
    std::atomic<std::size_t> passedCursor{0};
    std::atomic<std::uint64_t> publishedCount{0};
    std::atomic<std::uint64_t> usedCount{0};
    std::atomic<std::uint64_t> compiledCount{0};
    std::atomic<std::uint64_t> droppedCount{0};
};

}
