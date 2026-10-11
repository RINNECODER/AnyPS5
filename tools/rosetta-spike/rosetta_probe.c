// Probes the properties a directly executed PS5 title depends on, inside an
// x86_64 process. Build with `clang -arch x86_64 -O1` and run with
// `arch -x86_64`; build arm64 too to compare the native page behaviour.
#if defined(__x86_64__)
#include <cpuid.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static volatile char* g_page;
static volatile size_t g_len;
static volatile long g_faults;
static volatile uintptr_t g_fault_addr, g_fault_pc;

static void on_fault(int sig, siginfo_t* info, void* context) {
    (void)sig;
    ucontext_t* uc = context;
    g_fault_addr = (uintptr_t)info->si_addr;
#if defined(__x86_64__)
    g_fault_pc = uc->uc_mcontext->__ss.__rip;
#else
    g_fault_pc = uc->uc_mcontext->__ss.__pc;
#endif
    g_faults++;
    mprotect((void*)g_page, g_len, PROT_READ | PROT_WRITE);
}

static void probe_cpuid(void) {
#if defined(__x86_64__)
    unsigned a, b, c, d;
    char vendor[13] = {0};
    __cpuid(0, a, b, c, d);
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4);
    unsigned max = a;
    __cpuid(1, a, b, c, d);
    printf("cpuid vendor=%s max_leaf=%u leaf1 eax=%08x ecx=%08x edx=%08x\n", vendor, max, a, c, d);
    printf("  sse3=%d ssse3=%d fma=%d sse4.1=%d sse4.2=%d movbe=%d popcnt=%d aes=%d xsave=%d osxsave=%d avx=%d f16c=%d rdrand=%d\n",
           !!(c & 1), !!(c & (1 << 9)), !!(c & (1 << 12)), !!(c & (1 << 19)), !!(c & (1 << 20)), !!(c & (1 << 22)),
           !!(c & (1 << 23)), !!(c & (1 << 25)), !!(c & (1 << 26)), !!(c & (1 << 27)), !!(c & (1 << 28)),
           !!(c & (1 << 29)), !!(c & (1u << 30)));
    if (max >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        printf("  leaf7 ebx=%08x: bmi1=%d avx2=%d bmi2=%d adx=%d sha=%d\n", b, !!(b & (1 << 3)), !!(b & (1 << 5)),
               !!(b & (1 << 8)), !!(b & (1 << 19)), !!(b & (1 << 29)));
    }
    __cpuid(0x80000001, a, b, c, d);
    printf("  ext ecx=%08x lzcnt=%d sse4a=%d\n", c, !!(c & (1 << 5)), !!(c & (1 << 6)));
#endif
}

int main(void) {
    printf("arch=%s getpagesize=%d vm_page_size=%lu\n",
#if defined(__x86_64__)
           "x86_64",
#else
           "arm64",
#endif
           getpagesize(), (unsigned long)vm_page_size);
    int translated = 0;
    size_t sz = sizeof translated;
    sysctlbyname("sysctl.proc_translated", &translated, &sz, NULL, 0);
    printf("proc_translated=%d\n", translated);
    probe_cpuid();

    // 1. MAP_FIXED at 4K-aligned (not 16K-aligned) addresses.
    // 0. Which low ranges accept a fixed mapping at all (macOS reserves some).
    printf("fixed 16K map scan:");
    for (uintptr_t a = 0x100000000ull; a <= 0x20000000000ull; a <<= 1) {
        void* p = mmap((void*)a, 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        printf(" %#lx=%s", (unsigned long)a, p == MAP_FAILED ? (errno == EACCES ? "EACCES" : "ERR") : "ok");
        if (p != MAP_FAILED) munmap(p, 0x4000);
    }
    printf("\n");
    uintptr_t base = 0x20000000000ull;  // 2 TiB
    char* r = mmap((void*)base, 1 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    printf("fixed reserve 1MiB at %#lx: %s\n", (unsigned long)base, r == MAP_FAILED ? strerror(errno) : "ok");
    char* f4 = mmap((void*)(base + 0x1000), 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    printf("fixed 4K map at base+0x1000: %s\n", f4 == MAP_FAILED ? strerror(errno) : "ok");

    // 2. Large fixed reservations where PS5 titles put direct memory / heaps.
    uintptr_t big[] = {0x1000000000ull, 0x10000000000ull, 0x7e0000000000ull};
    for (unsigned i = 0; i < 3; i++) {
        size_t len = 64ull << 30;
        void* p = mmap((void*)big[i], len, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        printf("fixed reserve 64GiB at %#lx: %s\n", (unsigned long)big[i], p == MAP_FAILED ? strerror(errno) : "ok");
        if (p != MAP_FAILED) munmap(p, len);
    }

    // 3. mprotect granularity: protect one 4K page in the middle of a 16K page.
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    char* buf = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    char* aligned = (char*)(((uintptr_t)buf + 0x3fff) & ~(uintptr_t)0x3fff);
    int prc = mprotect(aligned + 0x1000, 0x1000, PROT_READ);
    printf("mprotect 4K inside 16K page: rc=%d\n", prc);
    g_page = aligned + 0x1000; g_len = 0x1000; g_faults = 0;
    aligned[0x0000] = 1;  // neighbour page: must not fault
    aligned[0x2000] = 1;  // neighbour page: must not fault
    long before = g_faults;
    aligned[0x1800] = 1;  // protected page: must fault once
    printf("  neighbour writes faults=%ld, protected write faults=%ld (expect 0 then 1) fault_addr_ok=%d\n", before,
           g_faults - before, g_fault_addr == (uintptr_t)(aligned + 0x1800));

    // 4. Write-fault round trip cost (write watch / guard pages).
    const int n = 20000;
    double t = now();
    for (int i = 0; i < n; i++) {
        mprotect((void*)g_page, 0x1000, PROT_READ);
        g_page[i & 0xfff] = (char)i;
    }
    t = now() - t;
    printf("write-fault round trip: %.2f us (%ld faults)\n", t / n * 1e6, g_faults);

    // 5. Aliased views of one shared object at 4K offsets (direct memory model).
    char name[64];
    snprintf(name, sizeof name, "/aps5probe.%d", getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    shm_unlink(name);
    int trc = ftruncate(fd, 0x10000);
    char* v1 = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char* v2 = mmap((void*)(base + 0x5000), 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0x3000);
    if (fd >= 0 && trc == 0 && v1 != MAP_FAILED && v2 != MAP_FAILED) {
        v1[0x3010] = 0x5a;
        printf("shm alias at 4K file offset into fixed 4K slot: ok, coherent=%d\n", v2[0x10] == 0x5a);
    } else {
        printf("shm alias at 4K file offset: failed (%s)\n", strerror(errno));
    }
    return 0;
}
