// Module the Rosetta runtime fixture loads at run time with
// sceKernelLoadStartModule; DT_INIT is its module_start.
typedef unsigned long u64;
#define EXPORT __attribute__((visibility("protected")))

static __thread volatile u64 dynamicTls __attribute__((tls_model("initial-exec"))) = 0x3333;
EXPORT volatile u64 DynamicStartCount;

EXPORT int DynamicStart(u64 args, const void* argp, void* param) {
    (void)param;
    ++DynamicStartCount;
    return (int)(args * 100 + (argp ? *(const unsigned char*)argp : 0));
}

// Returns the thread's previous value, which must start at 0x3333 on every thread.
EXPORT u64 DynamicProbe(u64 value) {
    const u64 previous = dynamicTls;
    dynamicTls = value;
    return previous;
}
