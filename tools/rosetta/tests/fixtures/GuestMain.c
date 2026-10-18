// Main image of the Rosetta runtime fixture. Runs under Aps5StartGuest and
// returns 0 when guest TLS and module loading behave; any other value names
// the first failed check (see tools/rosetta/tests/run_runtime_test.py).
typedef unsigned long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))

extern volatile u64 ModuleInitCount, ModuleInitArgs;
extern u64 ModuleSwapZero(u64 value);
extern u64 ModuleTlsGet(void);
extern void ModuleTlsSet(u64 value);

typedef struct { u64 st_size; char name[256]; int id; char rest[0x200]; } ModuleInfoEx;
extern int scePthreadCreate(void** thread, const void* attr, void* (*entry)(void*), void* arg, const char* name);
extern int scePthreadJoin(void* thread, void** value);
extern int scePthreadCancel(void* thread);
extern int sceKernelLoadStartModule(const char* path, u64 args, const void* argp, u32 flags, const void* option, int* result);
extern int sceKernelDlsym(int handle, const char* symbol, void** address);
extern int sceKernelGetModuleInfoFromAddr(u64 address, int flags, ModuleInfoEx* info);

static __thread volatile u64 mainTls __attribute__((tls_model("local-exec"))) = 0x1111;
static __thread volatile u64 mainTlsZero __attribute__((tls_model("local-exec")));

static u64 (*dynamicProbe)(u64);
static volatile u64* volatile mainTlsAddresses[5];

enum { Threads = 4 };
static u32 arrived;

// Every thread must see fresh initial values in every image, then keep its own.
static u64 CheckThread(u64 index) {
    if (mainTls != 0x1111 || mainTlsZero != 0) return 10;
    if (ModuleTlsGet() != 0x2222) return 11;
    if (ModuleSwapZero(0x500 + index) != 0) return 12;
    if (dynamicProbe && dynamicProbe(0x600 + index) != 0x3333) return 13;
    mainTls = 0x700 + index;
    mainTlsZero = index + 1;
    ModuleTlsSet(0x800 + index);
    mainTlsAddresses[index] = &mainTls;
    // Keep every thread (and its TLS blocks) alive until all have recorded theirs.
    if (index < Threads) {
        __atomic_add_fetch(&arrived, 1, __ATOMIC_SEQ_CST);
        for (u64 spin = 0; __atomic_load_n(&arrived, __ATOMIC_SEQ_CST) < Threads; ++spin) {
            if (spin > 4000000000ul) return 17;
            __builtin_ia32_pause();
        }
    }
    if (mainTls != 0x700 + index || mainTlsZero != index + 1 || ModuleTlsGet() != 0x800 + index) return 14;
    if (ModuleSwapZero(0) != 0x500 + index) return 15;
    if (dynamicProbe && dynamicProbe(0) != 0x600 + index) return 16;
    return 0;
}

static void* ThreadEntry(void* argument) {
    return (void*)CheckThread((u64)argument);
}

EXPORT int GuestMain(u64 argc, char** argv) {
    if (argc < 1 || !argv[0] || argv[argc]) return 2;
    // "fault": a guest NULL store, which the runtime must report with the guest RIP.
    if (argc > 1 && argv[1][0] == 'f') *(volatile u64*)(u64)argc = 0;
    if (ModuleInitCount != 1 || (ModuleInitArgs != 0 && ModuleInitArgs != 16)) return 3;
    // Initial thread first, then the dynamic module, then threads that see both.
    u64 status = CheckThread(Threads);
    if (status) return (int)status;
    static const unsigned char argument[] = {7, 0};
    int started = -1;
    const int handle = sceKernelLoadStartModule("/app0/dynamic/GuestDynamic.prx", 4, argument, 0, 0, &started);
    if (handle <= 0) return 20;
    if (started != 407) return 21;
    if (sceKernelDlsym(handle, "DynamicProbe", (void**)&dynamicProbe) != 0 || !dynamicProbe) return 22;
    if (dynamicProbe(0x900) != 0x3333 || dynamicProbe(0x3333) != 0x900) return 23;
    static ModuleInfoEx info;
    info.st_size = sizeof(ModuleInfoEx);
    if (sceKernelGetModuleInfoFromAddr((u64)dynamicProbe, 2, &info) != 0) return 24;
    if (info.id != handle) return 25;
    void* threads[Threads];
    for (u64 index = 0; index < Threads; ++index)
        if (scePthreadCreate(&threads[index], 0, ThreadEntry, (void*)index, "RosettaTls") != 0) return 30;
    for (u64 index = 0; index < Threads; ++index) {
        void* value = (void*)1;
        if (scePthreadJoin(threads[index], &value) != 0) return 31;
        if (value) return 40 + (int)(u64)value;
    }
    for (u64 index = 0; index <= Threads; ++index)
        for (u64 other = 0; other < index; ++other)
            if (mainTlsAddresses[index] == mainTlsAddresses[other]) return 32;
    if (mainTls != 0x700 + Threads || ModuleTlsGet() != 0x800 + Threads) return 33;
    // A joined thread's handle is stale: cancelling it is ESRCH, not a use of freed memory.
    if (scePthreadCancel(threads[0]) != (int)0x80020003) return 34;
    // The executable's PT_DYNAMIC is only read when a readable PT_LOAD covers it.
    if (sceKernelGetModuleInfoFromAddr((u64)GuestMain, 2, &info) != 0) return 35;
    return 0;
}
