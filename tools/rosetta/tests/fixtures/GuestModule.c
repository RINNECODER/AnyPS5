// Startup dependency of the Rosetta runtime fixture (DT_NEEDED by the main
// image, so dyld loads and initialises it before the guest entry). It reaches
// one TLS variable through __tls_get_addr and another through %fs.
typedef unsigned long u64;
#define EXPORT __attribute__((visibility("protected")))

static __thread volatile u64 moduleTls __attribute__((aligned(16), tls_model("global-dynamic"))) = 0x2222;
static __thread volatile u64 moduleTlsZero __attribute__((tls_model("initial-exec")));
EXPORT volatile u64 ModuleInitCount;
EXPORT volatile u64 ModuleInitArgs = ~0ul;

EXPORT int ModuleInit(u64 args, const void* argp, void* param) {
    (void)argp;
    (void)param;
    ++ModuleInitCount;
    ModuleInitArgs = args;
    return 0;
}

// Initial-exec access: the relinker rewrites the %fs:0 load into its TLS stub.
// Returns the thread's previous value, which must start at 0 on every thread.
EXPORT u64 ModuleSwapZero(u64 value) {
    const u64 previous = moduleTlsZero;
    moduleTlsZero = value;
    return previous;
}

// General-dynamic access: __tls_get_addr with this image's module descriptor.
EXPORT u64 ModuleTlsGet(void) {
    return moduleTls;
}

EXPORT void ModuleTlsSet(u64 value) {
    moduleTls = value;
}
