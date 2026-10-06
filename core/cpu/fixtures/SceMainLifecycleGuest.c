typedef unsigned long u64;
#define EXPORT __attribute__((visibility("protected")))

extern volatile u64 SceMainLifecycleState[13];

static void ctorA(void) {
    ++SceMainLifecycleState[3];
    SceMainLifecycleState[0] = SceMainLifecycleState[0] * 10 + 3;
    SceMainLifecycleState[9] = SceMainLifecycleState[9] * 5 + 2;
}

static void ctorB(void) {
    ++SceMainLifecycleState[4];
    SceMainLifecycleState[0] = SceMainLifecycleState[0] * 10 + 4;
    SceMainLifecycleState[9] = SceMainLifecycleState[9] * 7 + 3;
}

static void (*volatile constructors[])(void) = {ctorA, ctorB};

EXPORT int SceMainLifecycleInit(void) {
    ++SceMainLifecycleState[2];
    SceMainLifecycleState[0] = SceMainLifecycleState[0] * 10 + 2;
    for (u64 index = 0; index < sizeof constructors / sizeof constructors[0]; ++index)
        constructors[index]();
    return 0;
}

EXPORT void SceMainLifecycleFini(void) {
    if (SceMainLifecycleState[5] != 1 || SceMainLifecycleState[6] || SceMainLifecycleState[7] ||
        SceMainLifecycleState[8] || SceMainLifecycleState[11] != 3)
        SceMainLifecycleState[10] = 101;
    ++SceMainLifecycleState[6];
    SceMainLifecycleState[0] = SceMainLifecycleState[0] * 10 + 6;
    SceMainLifecycleState[9] = SceMainLifecycleState[9] * 13 + 5;
}

EXPORT void SceMainLifecycleRegisteredTermination(void) {
    if (SceMainLifecycleState[5] != 1 || SceMainLifecycleState[6] != 1 ||
        SceMainLifecycleState[7] || SceMainLifecycleState[8])
        SceMainLifecycleState[10] = 102;
    ++SceMainLifecycleState[7];
    SceMainLifecycleState[0] = SceMainLifecycleState[0] * 10 + 7;
    SceMainLifecycleState[9] = SceMainLifecycleState[9] * 17 + 6;
}

EXPORT int SceMainLifecycleMain(u64 argc, char** argv) {
    if (argc != 1 || !argv || !argv[0] || !*argv[0] || argv[argc]) return 81;
    if (SceMainLifecycleState[0] != 1234 || SceMainLifecycleState[1] != 1 ||
        SceMainLifecycleState[2] != 1 || SceMainLifecycleState[3] != 1 ||
        SceMainLifecycleState[4] != 1 || SceMainLifecycleState[5] ||
        SceMainLifecycleState[6] || SceMainLifecycleState[7] || SceMainLifecycleState[8] ||
        SceMainLifecycleState[9] != 262 || SceMainLifecycleState[10] ||
        SceMainLifecycleState[11] != 3 || SceMainLifecycleState[12]) return 83;
    ++SceMainLifecycleState[5];
    SceMainLifecycleState[0] = SceMainLifecycleState[0] * 10 + 5;
    SceMainLifecycleState[9] = SceMainLifecycleState[9] * 11 + 4;
    return 0;
}
