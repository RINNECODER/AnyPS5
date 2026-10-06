#include <metal_stdlib>
using namespace metal;

struct SampleCounterParameters {
    uint count;
    uint store;
};

kernel void SampleCounter(device uint* counter [[buffer(0)]], device uint* target [[buffer(1)]], constant SampleCounterParameters& params [[buffer(2)]]) {
    uint low = counter[0];
    uint high = counter[1];
    for (uint i = 0u; i < params.count; ++i) {
        uint addend = counter[4u + 2u * i];
        uint result = low + addend;
        uint carry = result < low ? 1u : 0u;
        low = result;
        high = high + counter[5u + 2u * i] + carry;
    }
    counter[0] = low;
    counter[1] = high;
    if (params.store != 0u) {
        for (uint db = 0u; db < 16u; ++db) {
            target[db * 4u] = db == 0u ? low : 0u;
            target[db * 4u + 1u] = (db == 0u ? high : 0u) | 0x80000000u;
        }
    }
}
