typedef unsigned long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
#define TLS_IE __attribute__((tls_model("initial-exec")))
extern u64 thread_self(void) __asm__("aI+OeCz8xrQ");
extern int* thread_error(void) __asm__("9BcDykPmo1I");

#if THREAD_DEPENDENCY

static u64 anchor = 0x59687786ul;
EXPORT __thread volatile u64 ThreadDependencyTls __attribute__((aligned(16))) = 0x8877665544332211ul;
EXPORT __thread u64* volatile ThreadDependencyPointer = &anchor;
EXPORT __thread volatile u64 ThreadDependencyZero[3];
EXPORT volatile u64 ThreadLifecycle[24];

EXPORT u64* ThreadDependencyAddress(void) { return (u64*)&ThreadDependencyTls; }
EXPORT int ThreadDependencyProbe(u64 expected, u64 expectedZero) {
    return ThreadDependencyTls == expected && ThreadDependencyZero[0] == expectedZero &&
        !ThreadDependencyZero[1] && !ThreadDependencyZero[2] &&
        ThreadDependencyPointer == &anchor && *ThreadDependencyPointer == 0x59687786ul;
}

EXPORT int ThreadDependencyInit(u64 args, u64 argp, u64 param) {
    ++ThreadLifecycle[0];
    ThreadLifecycle[3] = thread_self();
    ThreadLifecycle[4] = (u64)__builtin_thread_pointer();
    ThreadLifecycle[5] = (u64)thread_error();
    ThreadLifecycle[6] = (u32)*thread_error();
    ThreadLifecycle[7] = ThreadDependencyTls;
    ThreadLifecycle[8] = ThreadDependencyZero[0];
    ThreadLifecycle[9] = *ThreadDependencyPointer;
    ThreadLifecycle[17] = args;
    ThreadLifecycle[18] = argp;
    ThreadLifecycle[19] = param;
    return ThreadLifecycle[0] == 1 && !ThreadLifecycle[1] && !ThreadLifecycle[2] &&
        ThreadDependencyProbe(0x8877665544332211ul, 0) && !*thread_error() ? 0 : 91;
}

EXPORT int ThreadDependencyFini(u64 args, u64 argp, u64 param) {
    ++ThreadLifecycle[1];
    ThreadLifecycle[10] = thread_self();
    ThreadLifecycle[11] = (u64)__builtin_thread_pointer();
    ThreadLifecycle[12] = (u64)thread_error();
    ThreadLifecycle[13] = (u32)*thread_error();
    ThreadLifecycle[14] = ThreadDependencyTls;
    ThreadLifecycle[15] = ThreadDependencyZero[0];
    ThreadLifecycle[16] = *ThreadDependencyPointer;
    ThreadLifecycle[20] = args;
    ThreadLifecycle[21] = argp;
    ThreadLifecycle[22] = param;
    const int worker = ThreadLifecycle[2] == 3;
    if (ThreadLifecycle[0] != 1 || ThreadLifecycle[1] != 1 || (!worker && ThreadLifecycle[2] != 2) ||
        (worker ? (ThreadLifecycle[10] == ThreadLifecycle[3] || ThreadLifecycle[11] == ThreadLifecycle[4] ||
                   ThreadLifecycle[12] == ThreadLifecycle[5]) :
                  (ThreadLifecycle[10] != ThreadLifecycle[3] || ThreadLifecycle[11] != ThreadLifecycle[4] ||
                   ThreadLifecycle[12] != ThreadLifecycle[5])) ||
        !ThreadDependencyProbe(worker ? 0x8877665544332255ul : 0x8877665544332231ul, worker ? 13 : 7) ||
        *thread_error() != (worker ? 29 : 17)) { ThreadLifecycle[23] = 92; return 92; }
    return 0;
}

#else

extern __thread volatile u64 ThreadDependencyTls TLS_IE;
extern __thread u64* volatile ThreadDependencyPointer TLS_IE;
extern __thread volatile u64 ThreadDependencyZero[3] TLS_IE;
extern u64* ThreadDependencyAddress(void);
extern int ThreadDependencyProbe(u64, u64);
extern volatile u64 ThreadLifecycle[24];
extern void thread_dtors(void (*)(void)) __asm__("rNhWz+lvOMU");
extern void thread_count(int (*)(u32)) __asm__("pB-yGZ2nQ9o");
extern void thread_report(void (*)(u32)) __asm__("WhCc1w3EhSI");
extern int thread_create(u64*, const u64*, void* (*)(void*), void*, const char*) __asm__("6UgtwV+0zb4");
extern void thread_yield(void) __asm__("T72hz6ffq08");
extern int thread_join(u64, void**) __asm__("onNY9Byn-W8");
extern int thread_equal(u64, u64) __asm__("3PtV6p3QNX4");
extern void thread_exit(void*) __asm__("3kg7rT0NQIs");
extern void process_exit(int) __asm__("uMei1W9uyNo");
extern void kernel_exit(int) __asm__("6Z83sYWFlA8");

static u64 anchor = 0x69887796ul;
EXPORT __thread volatile u64 ThreadMainTls __attribute__((aligned(16))) TLS_IE = 0x1122334455667788ul;
EXPORT __thread u64* volatile ThreadMainPointer TLS_IE = &anchor;
EXPORT __thread volatile u64 ThreadMainZero[3] TLS_IE;
EXPORT __thread volatile u64 ThreadMainGd = 0x2233445566778899ul;
EXPORT volatile u64 ThreadReceipt[36];
EXPORT volatile u64 ThreadEvents[6];
static volatile u64 iterations = 8192;
static volatile u64 exitChild;

static void record(u64 event) {
    const u64 index = ThreadReceipt[0];
    if (index >= 6) { ThreadReceipt[1] = 101; return; }
    ThreadEvents[index] = event;
    ThreadReceipt[0] = index + 1;
}

static int tls(u64 main, u64 dependency, u64 zero, u64 gd) {
    return ThreadMainTls == main && ThreadMainZero[0] == zero && !ThreadMainZero[1] &&
        !ThreadMainZero[2] && ThreadMainGd == gd && ThreadMainPointer == &anchor &&
        *ThreadMainPointer == 0x69887796ul && ThreadDependencyTls == dependency &&
        ThreadDependencyZero[0] == zero && ThreadDependencyPointer &&
        *ThreadDependencyPointer == 0x59687786ul &&
        ThreadDependencyAddress() == (u64*)&ThreadDependencyTls && ThreadDependencyProbe(dependency, zero);
}

EXPORT int ThreadCount(u32 module) { ++ThreadReceipt[10]; ThreadReceipt[31] = module; return 17; }
EXPORT void ThreadReport(u32 module) { ++ThreadReceipt[11]; ThreadReceipt[32] = module; }

EXPORT void ThreadDestructor(void) {
    if (!tls(0x11223344556677bbul, 0x8877665544332255ul, 13, 0x22334455667788d9ul) ||
        *thread_error() != 29 || !thread_equal(thread_self(), ThreadReceipt[7])) ThreadReceipt[1] = 102;
    ThreadMainTls += 9;
    ThreadDependencyTls += 11;
    ThreadMainZero[0] += 2;
    ThreadDependencyZero[0] += 2;
    *thread_error() = 44;
    ThreadReceipt[9] += 1;
    ThreadReceipt[18] = ThreadMainTls;
    ThreadReceipt[19] = ThreadDependencyTls;
    ThreadReceipt[20] = ThreadMainZero[0];
    ThreadReceipt[21] = (u32)*thread_error();
    ThreadReceipt[34] = ThreadMainGd;
    record(5);
    ThreadReceipt[8] = thread_self();
}

EXPORT __attribute__((noinline)) u64 ThreadArithmetic(void) {
    typedef float v4 __attribute__((vector_size(16)));
    v4 vector = {0, 0, 0, 0};
    const v4 increment = {1, 2, 3, 4};
    long double extended = 0;
    u64 scalar = 0;
    const u64 limit = iterations;
    for (u64 index = 1; index <= limit; ++index) {
        scalar += index;
        vector += increment;
        extended += 0.5L;
    }
    return scalar + (u64)vector[0] + (u64)vector[1] + (u64)vector[2] + (u64)vector[3] + (u64)extended;
}

EXPORT void* ThreadChild(void* argument) {
    volatile u64 canary[32];
    for (u64 index = 0; index < 32; ++index) canary[index] = 0xabcdef0100000000ul + index * 37;
    ThreadReceipt[3] = (u64)__builtin_thread_pointer();
    ThreadReceipt[5] = (u64)thread_error();
    ThreadReceipt[7] = thread_self();
    ThreadReceipt[26] = (u64)argument;
    ThreadReceipt[28] = (u64)&ThreadMainGd;
    ThreadReceipt[30] = (u64)ThreadDependencyAddress();
    if ((u64)argument != 0xabcdef0198765432ul || *thread_error() ||
        !tls(0x1122334455667788ul, 0x8877665544332211ul, 0, 0x2233445566778899ul)) ThreadReceipt[1] = 103;
    ThreadMainTls += 0x33;
    ThreadDependencyTls += 0x44;
    ThreadMainGd += 0x40;
    ThreadMainZero[0] = ThreadDependencyZero[0] = 13;
    *thread_error() = 29;
    record(2);
    thread_yield();
    if (!tls(0x11223344556677bbul, 0x8877665544332255ul, 13, 0x22334455667788d9ul) ||
        *thread_error() != 29 || thread_self() != ThreadReceipt[7]) ThreadReceipt[1] = 104;
    ThreadReceipt[12] = ThreadArithmetic();
    for (u64 index = 0; index < 32; ++index)
        if (canary[index] != 0xabcdef0100000000ul + index * 37) ThreadReceipt[1] = 105;
    record(4);
    if (exitChild == 15) {
        volatile u64* const returnWord = (volatile u64*)ThreadReceipt[31];
        if (!returnWord) { ThreadReceipt[1] = 108; return (void*)0; }
        ThreadReceipt[32] = *returnWord;
        *returnWord = 0x1122334455667788ul;
    }
    if (exitChild == 13 || exitChild == 14) {
        ThreadLifecycle[2] = 3;
        if (exitChild == 13) process_exit(23);
        else kernel_exit(24);
        ThreadReceipt[1] = 107;
        return (void*)0x1122334455667788ul;
    }
    if (exitChild == 12) {
        thread_exit((void*)0x8877665544332211ul);
        ThreadReceipt[1] = 106;
        return (void*)0x1122334455667788ul;
    }
    return (void*)0x8877665544332211ul;
}

__attribute__((naked, noinline)) static int joinWithPublishedReturn(u64 target, void** output) {
    __asm__ volatile(
        "sub $8, %rsp\n"
        "lea -8(%rsp), %rax\n"
        "mov %rax, ThreadReceipt+248(%rip)\n"
        "call \"onNY9Byn-W8\"@PLT\n"
        "add $8, %rsp\n"
        "ret\n");
}

static int number(const char* text, u64* value) {
    if (!text || !*text) return 0;
    *value = 0;
    u64 count = 0;
    while (*text) {
        if (++count > 10 || *text < '0' || *text > '9') return 0;
        *value = *value * 10 + (unsigned)(*text++ - '0');
    }
    return 1;
}

EXPORT int SceGuestMain(u64 argc, char** argv) {
    u64 mode, expected;
    if (argc != 3 || argv[argc] || !number(argv[1], &mode) || !number(argv[2], &expected)) return 81;
    if (ThreadLifecycle[0] != 1 || ThreadLifecycle[1] || ThreadLifecycle[2]) return 93;
    ThreadLifecycle[2] = 1;
    if (mode == 2) thread_dtors((void (*)(void))0);
    thread_dtors(ThreadDestructor);
    if (mode == 3) thread_dtors(ThreadDestructor);
    thread_count(ThreadCount);
    thread_report(ThreadReport);
    if (ThreadReceipt[9] || ThreadReceipt[10] || ThreadReceipt[11]) return 82;
    if (!tls(0x1122334455667788ul, 0x8877665544332211ul, 0, 0x2233445566778899ul) || *thread_error()) return 83;
    ThreadReceipt[2] = (u64)__builtin_thread_pointer();
    ThreadReceipt[4] = (u64)thread_error();
    ThreadReceipt[6] = thread_self();
    ThreadReceipt[27] = (u64)&ThreadMainGd;
    ThreadReceipt[29] = (u64)ThreadDependencyAddress();
    ThreadMainTls += 0x10;
    ThreadDependencyTls += 0x20;
    ThreadMainGd += 0x30;
    ThreadMainZero[0] = ThreadDependencyZero[0] = 7;
    *thread_error() = 17;
    volatile u64 canary[32];
    for (u64 index = 0; index < 32; ++index) canary[index] = 0x1234567800000000ul + index * 41;
    record(1);
    exitChild = mode >= 12 && mode <= 15 ? mode : 0;
    if (mode == 8) thread_join(thread_self(), (void**)0);
    if (mode == 9) thread_join(0xfedcba9876543210ul, (void**)0);
    volatile struct { u64 before, handle, after; } created = {0x13579bdf2468ace0ul, 0, 0xeca86420fdb97531ul};
    u64* output = mode == 5 ? (u64*)0x300000000ffful : (u64*)&created.handle;
    const u64 attribute = 0;
    const u64* attr = mode == 6 ? &attribute : (const u64*)0;
    void* (*entry)(void*) = mode == 4 ? (void* (*)(void*))&anchor : ThreadChild;
    if (thread_create(output, attr, entry, (void*)0xabcdef0198765432ul, (const char*)0) != 0) return 84;
    ThreadReceipt[22] = created.before;
    ThreadReceipt[23] = created.after;
    if (!created.handle || created.before != 0x13579bdf2468ace0ul || created.after != 0xeca86420fdb97531ul) return 85;
    if (mode == 7) thread_join(created.handle, (void**)0x300000000ffful);
    thread_yield();
    if (!tls(0x1122334455667798ul, 0x8877665544332231ul, 7, 0x22334455667788c9ul) ||
        *thread_error() != 17 || thread_self() != ThreadReceipt[6] || ThreadReceipt[0] != 2) return 86;
    for (u64 index = 0; index < 32; ++index)
        if (canary[index] != 0x1234567800000000ul + index * 41) return 87;
    record(3);
    volatile struct { u64 before; void* result; u64 after; } joined = {
        0x1020304050607080ul, (void*)0xfeedfacefeedfaceul, 0x8070605040302010ul};
    void** joinedOutput = mode == 11 ? (void**)ThreadReceipt[5] : (void**)&joined.result;
    if (mode == 15) {
        ThreadReceipt[35] = (u64)&joined;
        if (joinWithPublishedReturn(created.handle, joinedOutput) != 0) return 88;
    } else if (thread_join(created.handle, joinedOutput) != 0) return 88;
    if (mode == 10) thread_join(created.handle, (void**)0);
    ThreadReceipt[13] = (u64)joined.result;
    ThreadReceipt[24] = joined.before;
    ThreadReceipt[25] = joined.after;
    if ((u64)joined.result != 0x8877665544332211ul || joined.before != 0x1020304050607080ul ||
        joined.after != 0x8070605040302010ul || ThreadReceipt[9] != 1 ||
        ThreadReceipt[8] != created.handle || ThreadReceipt[10] || ThreadReceipt[11]) return 89;
    if (!tls(0x1122334455667798ul, 0x8877665544332231ul, 7, 0x22334455667788c9ul) || *thread_error() != 17) return 90;
    for (u64 index = 0; index < 32; ++index)
        if (canary[index] != 0x1234567800000000ul + index * 41) return 91;
    ThreadReceipt[14] = ThreadMainTls;
    ThreadReceipt[15] = ThreadDependencyTls;
    ThreadReceipt[16] = ThreadMainZero[0];
    ThreadReceipt[17] = (u32)*thread_error();
    ThreadReceipt[33] = ThreadMainGd;
    record(6);
    if (ThreadReceipt[1]) return (int)ThreadReceipt[1];
    ThreadLifecycle[2] = 2;
    return ThreadReceipt[12] == expected ? 0 : 77;
}

#endif
