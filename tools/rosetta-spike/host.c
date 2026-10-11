// Runs kernel() natively (arm64) or under Rosetta (x86_64 slice) and reports
// wall time. `host --dump <file>` writes the kernel's code bytes for bench_tcg.py.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t kernel(uint64_t iters, uint32_t* buf, double scale);

static uint32_t buf[4096];

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "--dump")) {
        FILE* f = fopen(argv[2], "wb");
        if (!f) return 1;
        fwrite((const void*)kernel, 1, 1024, f);
        fclose(f);
        return 0;
    }
    uint64_t iters = argc > 1 ? strtoull(argv[1], NULL, 0) : 100000000ull;
    for (int i = 0; i < 4096; i++) buf[i] = (uint32_t)(i * 2654435761u);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t r = kernel(iters, buf, 0.5);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
    printf("iters=%llu result=%016llx seconds=%.4f\n", (unsigned long long)iters, (unsigned long long)r, s);
    return 0;
}
