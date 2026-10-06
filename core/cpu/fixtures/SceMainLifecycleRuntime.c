typedef unsigned long u64;
#define EXPORT __attribute__((visibility("protected")))

extern void kernel_exit(int) __asm__("6Z83sYWFlA8") __attribute__((noreturn));
EXPORT volatile u64 SceMainLifecycleState[13] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0};
static void (*callbacks[16])(void);
static u64 callbackCount;

EXPORT int SceMainLifecycleDependencyInit(u64 args, const void* argp, void* param) {
    (void)args;
    (void)argp;
    (void)param;
    ++SceMainLifecycleState[1];
    SceMainLifecycleState[0] = SceMainLifecycleState[0] * 10 + 1;
    SceMainLifecycleState[9] = SceMainLifecycleState[9] * 3 + 1;
    return 0;
}

EXPORT int SceMainLifecycleDependencyFini(u64 args, const void* argp, void* param) {
    (void)args;
    (void)argp;
    (void)param;
    if (SceMainLifecycleState[5] != 1 || SceMainLifecycleState[6] != 1 ||
        SceMainLifecycleState[7] != 1 || SceMainLifecycleState[12] != 1)
        SceMainLifecycleState[10] = 103;
    ++SceMainLifecycleState[8];
    SceMainLifecycleState[0] = SceMainLifecycleState[0] * 10 + 8;
    SceMainLifecycleState[9] = SceMainLifecycleState[9] * 19 + 7;
    return 0;
}

EXPORT int atexit(void (*callback)(void)) __asm__("8G2LB+A3rzg");
EXPORT int atexit(void (*callback)(void)) {
    if (!callback || callbackCount == 16 || SceMainLifecycleState[12]) return 1;
    callbacks[callbackCount++] = callback;
    ++SceMainLifecycleState[11];
    return 0;
}

EXPORT void exit(int status) __asm__("uMei1W9uyNo") __attribute__((noreturn));
EXPORT void exit(int status) {
    volatile u64 frame[96];
    for (u64 index = 0; index < sizeof frame / sizeof frame[0]; ++index)
        frame[index] = 0x9e3779b97f4a7c15ul ^ (index * 0x100000001b3ul);
    if (SceMainLifecycleState[5] != 1 || SceMainLifecycleState[6] || SceMainLifecycleState[7] ||
        SceMainLifecycleState[8] || SceMainLifecycleState[12])
        SceMainLifecycleState[10] = 104;
    ++SceMainLifecycleState[12];
    while (callbackCount) {
        callbacks[--callbackCount]();
        for (u64 index = 0; index < sizeof frame / sizeof frame[0]; ++index)
            if (frame[index] != (0x9e3779b97f4a7c15ul ^ (index * 0x100000001b3ul)))
                SceMainLifecycleState[10] = 108;
    }
    if (SceMainLifecycleState[8] != 1 || SceMainLifecycleState[0] != 12345678)
        SceMainLifecycleState[10] = 107;
    for (u64 index = 0; index < sizeof frame / sizeof frame[0]; ++index)
        if (frame[index] != (0x9e3779b97f4a7c15ul ^ (index * 0x100000001b3ul)))
            SceMainLifecycleState[10] = 108;
    if (SceMainLifecycleState[10]) status = (int)SceMainLifecycleState[10];
    kernel_exit(status);
}
