// Guest-like hot loop for the Rosetta vs TCG comparison. Self-contained: no
// calls, no literal pools, no jump tables, so its bytes can be lifted out of
// the host binary and executed by a TCG engine unchanged.
#include <stdint.h>

uint64_t kernel(uint64_t iters, uint32_t* buf, double scale) {
    uint64_t h = 1469598103934665603ull;
    double acc = 0;
    for (uint64_t i = 0; i < iters; i++) {
        uint32_t idx = (uint32_t)(h >> 7) & 4095;
        uint32_t v = buf[idx];
        h ^= v + i;
        h *= 1099511628211ull;
        buf[(idx * 33) & 4095] = (uint32_t)h;
        if (h & 1)
            acc += (double)(v & 0xffff) * scale;
        else
            acc -= scale;
    }
    return h ^ (uint64_t)(int64_t)acc;
}
