#include "MetalTestSupport.hpp"
#include <array>
#include <cstring>

namespace {
struct SampleParameters {
    uint32_t count;
    uint32_t store;
};

struct SampleCase {
    uint32_t low;
    uint32_t high;
    std::array<uint32_t, 6> samples;
    SampleParameters parameters;
    uint32_t expectedLow;
    uint32_t expectedHigh;
};
}

void RunSampleCounterTests(const MetalTests::Context& context) {
    const SampleCase cases[] = {
        {0x12345678u, 0x76543210u, {1, 2, 3, 4, 5, 6}, {0, 0}, 0x12345678u, 0x76543210u},
        {0x12345678u, 0x76543210u, {1, 2, 3, 4, 5, 6}, {0, 1}, 0x12345678u, 0x76543210u},
        {0xffffffffu, 0, {2, 0, 3, 4, 5, 6}, {1, 0}, 1, 1},
        {0xffffffffu, 0xffffffffu, {1, 0, 3, 4, 5, 6}, {1, 1}, 0, 0},
        {0xfffffff0u, 0xfffffffeu, {0x20, 1, 0xffffffffu, 0xffffffffu, 2, 0}, {3, 7}, 0x11, 0},
        {7, 0x80000000u, {0, 5, 3, 4, 5, 6}, {1, 1}, 7, 0x80000005u},
    };
    for (const auto& test : cases) {
        std::array<uint32_t, 12> counter = {test.low, test.high, 0x11223344u, 0x55667788u, test.samples[0], test.samples[1], test.samples[2], test.samples[3], test.samples[4], test.samples[5], 0xfeedfaceu, 0xcafebabeu};
        auto expectedCounter = counter;
        expectedCounter[0] = test.expectedLow;
        expectedCounter[1] = test.expectedHigh;
        auto counterBuffer = context.Buffer(sizeof(counter));
        auto targetBuffer = context.Buffer(68 * sizeof(uint32_t), 0xa5);
        std::memcpy([counterBuffer contents], counter.data(), sizeof(counter));
        context.Dispatch(@"SampleCounter", @[counterBuffer, targetBuffer], &test.parameters, sizeof(test.parameters), MTLSizeMake(1, 1, 1));
        MetalTests::Require(std::memcmp([counterBuffer contents], expectedCounter.data(), sizeof(expectedCounter)) == 0, "SampleCounter accumulated incorrectly or modified reserved/sample words");
        const auto* target = static_cast<const uint32_t*>([targetBuffer contents]);
        for (size_t i = 0; i < 68; ++i) {
            uint32_t expected = 0xa5a5a5a5u;
            if (test.parameters.store != 0 && i < 64) {
                if (i % 4 == 0) expected = i == 0 ? test.expectedLow : 0;
                if (i % 4 == 1) expected = (i == 1 ? test.expectedHigh : 0) | 0x80000000u;
            }
            MetalTests::Require(target[i] == expected, "SampleCounter store layout or preservation of untouched words is incorrect");
        }
    }
}
