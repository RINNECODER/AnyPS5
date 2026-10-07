// Freestanding compiled x86-64 caller; contains no host-side result oracle.
typedef unsigned long long u64;
typedef int (*Gate)(u64, u64, u64);
__attribute__((section(".text"), visibility("default")))
void KernelProbe(const Gate* g, volatile u64* out, unsigned mode) {
    volatile u64* mutex = (volatile u64*)0x2200;
    volatile u64* attr = (volatile u64*)0x2210;
    if (mode) { out[0] = (unsigned)g[mode - 1](out[32], out[33], out[34]); return; }
    *mutex = 0; *attr = 0;
    out[0] = (unsigned)g[4](0x2210, 0, 0);
    out[1] = (unsigned)g[5](0x2210, 2, 0);
    out[2] = (unsigned)g[0](0x2200, 0x2210, 0x2250);
    out[3] = (unsigned)g[1](0x2200, 0, 0);
    out[4] = (unsigned)g[1](0x2200, 0, 0);
    out[5] = (unsigned)g[3](0x2200, 0, 0);
    out[6] = (unsigned)g[2](0x2200, 0, 0);
    out[7] = (unsigned)g[3](0x2200, 0, 0);
    out[8] = (unsigned)g[2](0x2200, 0, 0);
    out[9] = (unsigned)g[3](0x2200, 0, 0);
    out[10] = *mutex;
    out[11] = (unsigned)g[3](0x2200, 0, 0);
    out[12] = (unsigned)g[6](0x2210, 0, 0);
    out[13] = *attr;
    out[14] = (unsigned)g[0](0x2200, 0, 0);
    out[15] = (unsigned)g[8](0x2200, 0, 0);
    out[16] = (unsigned)g[1](0x2200, 0, 0);
    out[17] = (unsigned)g[8](0x2200, 0, 0);
    out[18] = (unsigned)g[2](0x2200, 0, 0);
    out[19] = (unsigned)g[3](0x2200, 0, 0);
    *mutex = 0;
    out[20] = (unsigned)g[1](0x2200, 0, 0);
    out[21] = (unsigned)g[2](0x2200, 0, 0);
    out[22] = (unsigned)g[3](0x2200, 0, 0);
    out[23] = *mutex;
}
