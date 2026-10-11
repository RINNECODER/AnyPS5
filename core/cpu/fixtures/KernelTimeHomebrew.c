// Synthetic guest for the libkernel time services (#278). The main image calls every
// clock, counter and sleep through real PLT imports in the libkernel and libScePosix
// scopes, and runs a second guest thread to show that a sleep parks only its caller.
typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))

#if BUILD_KERNEL_TIME_DEPENDENCY

static volatile u32 anchor = 3;
static volatile u32* volatile relocatedAnchor = &anchor;

EXPORT u32 KernelTimeGuestValue(void) { return *relocatedAnchor; }

#else

struct timespec { i64 tv_sec; i64 tv_nsec; };
struct timeval { i64 tv_sec; i64 tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
struct timesec { i64 t; u32 west_sec; u32 dst_sec; };

extern u32 KernelTimeGuestValue(void);
extern int sceKernelUsleep(u32);
extern int sceKernelNanosleep(const struct timespec*, struct timespec*);
extern int sceKernelSleep(u32);
extern u64 sceKernelGetProcessTime(void);
extern u64 sceKernelGetProcessTimeCounter(void);
extern u64 sceKernelGetProcessTimeCounterFrequency(void);
extern u64 sceKernelReadTsc(void);
extern u64 sceKernelGetTscFrequency(void);
extern int sceKernelGettimeofday(struct timeval*);
extern int sceKernelClockGettime(int, struct timespec*);
extern int sceKernelClockGetres(int, struct timespec*);
extern int sceKernelConvertUtcToLocaltime(i64, i64*, struct timesec*, u64*);
extern int sceKernelConvertLocaltimeToUtc(i64, i64, i64*, struct timezone*, int*);
extern int clock_gettime(int, struct timespec*);
extern int clock_getres(int, struct timespec*);
extern int gettimeofday(struct timeval*, struct timezone*);
extern int nanosleep(const struct timespec*, struct timespec*);
extern int usleep(u32);
extern u32 sleep(u32);
extern int thread_create(u64*, const u64*, void* (*)(void*), void*, const char*) __asm__("6UgtwV+0zb4");
extern int thread_join(u64, void**) __asm__("onNY9Byn-W8");
extern int* thread_error(void) __asm__("9BcDykPmo1I");

#define SCE_EFAULT ((int)0x8002000e)
#define SCE_EINVAL ((int)0x80020016)

static volatile u64 sleeping, finished, workerSamples, workerDuringSleep;

static int number(const char* text, u64* result) {
    if (!text || !*text) return 0;
    u64 value = 0, length = 0;
    while (*text) {
        if (++length > 19 || *text < '0' || *text > '9') return 0;
        value = value * 10 + (unsigned)(*text++ - '0');
    }
    *result = value;
    return 1;
}

static int same(const char* left, const char* right) {
    while (*left && *left == *right) ++left, ++right;
    return *left == *right;
}

static u64 rdtsc(void) {
    u32 low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((u64)high << 32) | low;
}

static i64 distance(i64 left, i64 right) { return left > right ? left - right : right - left; }

// Every counter must advance at the rate its own frequency claims, measured against
// process time across a real 20 ms sleep (tolerance 5 ms for host scheduling).
static int clocks(u64 hostEpoch) {
    const u64 frequency = sceKernelGetProcessTimeCounterFrequency();
    const u64 tscFrequency = sceKernelGetTscFrequency();
    if (!frequency || tscFrequency < 1000000ul) return 10;
    const u64 p0 = sceKernelGetProcessTime(), c0 = sceKernelGetProcessTimeCounter(), r0 = sceKernelReadTsc();
    const u64 guest = rdtsc();
    const u64 r1 = sceKernelReadTsc();
    if (!(r0 < guest && guest < r1)) return 11;
    if (sceKernelUsleep(20000) != 0) return 12;
    const u64 p1 = sceKernelGetProcessTime(), c1 = sceKernelGetProcessTimeCounter(), r2 = sceKernelReadTsc();
    const i64 elapsed = (i64)(p1 - p0);
    if (elapsed < 20000 || elapsed > 2000000) return 13;
    if (distance((i64)((c1 - c0) / (frequency / 1000000ul)), elapsed) > 5000) return 14;
    if (distance((i64)((r2 - r0) / (tscFrequency / 1000000ul)), elapsed) > 5000) return 15;

    static const int valid[] = {0, 1, 2, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (unsigned index = 0; index < sizeof(valid) / sizeof(valid[0]); ++index) {
        struct timespec value = {-1, -1}, resolution = {-1, -1};
        if (sceKernelClockGettime(valid[index], &value) != 0) return 20;
        if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000l) return 21;
        if (valid[index] == 13 && value.tv_nsec) return 22;
        if (sceKernelClockGetres(valid[index], &resolution) != 0 || resolution.tv_sec < 0 ||
            resolution.tv_nsec < 0 || resolution.tv_nsec >= 1000000000l || (!resolution.tv_sec && !resolution.tv_nsec)) return 23;
    }
    struct timespec realtime, monotonic0, monotonic1, posix;
    if (sceKernelClockGettime(0, &realtime) || distance(realtime.tv_sec, (i64)hostEpoch) > 120) return 24;
    if (sceKernelClockGettime(4, &monotonic0) || sceKernelUsleep(10000) || sceKernelClockGettime(4, &monotonic1)) return 25;
    const i64 monotonicNanos = (monotonic1.tv_sec - monotonic0.tv_sec) * 1000000000l + monotonic1.tv_nsec - monotonic0.tv_nsec;
    if (monotonicNanos < 10000000l || monotonicNanos > 2000000000l) return 26;
    if (sceKernelClockGettime(3, &posix) != SCE_EINVAL || sceKernelClockGettime(0, 0) != SCE_EFAULT) return 27;
    if (sceKernelClockGetres(999, &posix) != SCE_EINVAL) return 28;

    *thread_error() = 0;
    if (clock_gettime(4, &posix) != 0 || posix.tv_sec < monotonic1.tv_sec || *thread_error()) return 30;
    if (clock_gettime(999, &posix) != -1 || *thread_error() != 22) return 31;
    *thread_error() = 0;
    if (clock_getres(0, &posix) != 0 || clock_getres(-5, &posix) != -1 || *thread_error() != 22) return 32;

    struct timeval tv = {-1, -1}, posixTv = {-1, -1};
    struct timezone tz = {77, 77};
    if (sceKernelGettimeofday(&tv) != 0 || distance(tv.tv_sec, (i64)hostEpoch) > 120 || tv.tv_usec < 0 ||
        tv.tv_usec >= 1000000l) return 40;
    if (sceKernelGettimeofday(0) != SCE_EFAULT) return 41;
    if (gettimeofday(&posixTv, &tz) != 0 || posixTv.tv_sec < tv.tv_sec || tz.tz_minuteswest || tz.tz_dsttime) return 42;

    i64 local = 0, utc = 0;
    struct timesec seconds = {0, 9, 9};
    u64 dst = 9;
    struct timezone zone = {9, 9};
    int dstSeconds = 9;
    if (sceKernelConvertUtcToLocaltime(tv.tv_sec, &local, &seconds, &dst) != 0 || local != tv.tv_sec ||
        seconds.t != tv.tv_sec || seconds.west_sec || seconds.dst_sec || dst) return 50;
    if (sceKernelConvertLocaltimeToUtc(local, 0, &utc, &zone, &dstSeconds) != 0 || utc != local ||
        zone.tz_minuteswest || zone.tz_dsttime || dstSeconds) return 51;
    return 0;
}

// The scheduler is FIFO on one host thread, so the worker sleeps between samples
// rather than spinning; two guest threads are then asleep at once.
static void* worker(void* argument) {
    (void)argument;
    while (!finished) {
        const u64 now = sceKernelGetProcessTime();
        ++workerSamples;
        if (sleeping) workerDuringSleep = now;
        if (sceKernelUsleep(1000) != 0) return 0;
    }
    return (void*)0x5157;
}

// Held in a relocated data slot, so the image carries a genuine RELATIVE relocation.
static void* (*volatile workerEntry)(void*) = worker;

static int elapsedAtLeast(u64 start, u64 microseconds) {
    const u64 elapsed = sceKernelGetProcessTime() - start;
    return elapsed >= microseconds && elapsed < microseconds + 2000000ul;
}

// A sleeping thread must let the other guest thread run for the whole sleep.
static int sleeps(void) {
    u64 handle = 0;
    if (thread_create(&handle, 0, workerEntry, 0, "time-worker") != 0 || !handle) return 60;
    const u64 start = sceKernelGetProcessTime();
    sleeping = 1;
    if (sceKernelUsleep(80000) != 0) return 61;
    sleeping = 0;
    const u64 end = sceKernelGetProcessTime();
    if (end - start < 80000) return 62;
    // The worker sampled the clock well inside the sleep, not just at its edges.
    if (workerDuringSleep < start + 20000 || workerDuringSleep > end) return 63;
    finished = 1;
    void* result = 0;
    if (thread_join(handle, &result) != 0 || result != (void*)0x5157 || !workerSamples) return 64;

    struct timespec request = {0, 30000000l}, remaining = {7, 7};
    u64 begin = sceKernelGetProcessTime();
    if (sceKernelNanosleep(&request, &remaining) != 0 || remaining.tv_sec || remaining.tv_nsec ||
        !elapsedAtLeast(begin, 30000)) return 65;
    begin = sceKernelGetProcessTime();
    if (nanosleep(&request, 0) != 0 || !elapsedAtLeast(begin, 30000)) return 66;
    begin = sceKernelGetProcessTime();
    if (usleep(25000) != 0 || !elapsedAtLeast(begin, 25000)) return 67;
    begin = sceKernelGetProcessTime();
    if (sceKernelSleep(1) != 0 || !elapsedAtLeast(begin, 1000000)) return 68;
    begin = sceKernelGetProcessTime();
    if (sleep(1) != 0 || !elapsedAtLeast(begin, 1000000)) return 69;
    if (sceKernelUsleep(0) != 0 || usleep(0) != 0) return 70;

    struct timespec invalid = {0, 1000000000l}, negative = {-1, 0};
    if (sceKernelNanosleep(0, 0) != SCE_EFAULT || sceKernelNanosleep(&invalid, 0) != SCE_EINVAL) return 71;
    *thread_error() = 0;
    if (nanosleep(&invalid, 0) != -1 || *thread_error() != 22) return 72;
    if (sceKernelNanosleep(&negative, 0) != 0) return 73;
    return 0;
}

EXPORT int SceGuestMain(u64 argc, char** argv) {
    if (argc < 2 || !argv[1] || KernelTimeGuestValue() != 3) return 90;
    u64 epoch = 0;
    if (same(argv[1], "clocks")) return argc == 3 && number(argv[2], &epoch) ? clocks(epoch) : 91;
    if (same(argv[1], "sleeps")) return sleeps();
    if (same(argv[1], "forever")) { sceKernelSleep(3600); return 92; }
    return 93;
}

#endif
