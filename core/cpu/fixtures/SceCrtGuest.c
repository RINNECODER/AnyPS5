typedef unsigned long u64;
#define EXPORT __attribute__((visibility("protected")))

EXPORT volatile u64 SceCrtState[9] = {0, 0, 0, 0, 0, 0, 0, 5, 0};

static void ctorA(void) {
    ++SceCrtState[3];
    SceCrtState[0] = SceCrtState[0] * 10 + 2;
    SceCrtState[7] = SceCrtState[7] * 3 + 1;
}

static void ctorB(void) {
    ++SceCrtState[4];
    SceCrtState[0] = SceCrtState[0] * 10 + 3;
    SceCrtState[7] = SceCrtState[7] * 5 + 2;
}

static void dtorA(void) {
    ++SceCrtState[5];
    SceCrtState[0] = SceCrtState[0] * 10 + 6;
    SceCrtState[7] = SceCrtState[7] * 13 + 7;
}

static void dtorB(void) {
    ++SceCrtState[6];
    SceCrtState[0] = SceCrtState[0] * 10 + 7;
    SceCrtState[7] = SceCrtState[7] * 11 + 5;
}

static void (*volatile initCallbacks[])(void) __attribute__((section(".init_array"), used)) = {ctorA, ctorB};
static void (*volatile finiCallbacks[])(void) __attribute__((section(".fini_array"), used)) = {dtorA, dtorB};

EXPORT int SceCrtInit(u64 args, const void* argp, void* param) {
    (void)args;
    (void)argp;
    (void)param;
    ++SceCrtState[1];
    SceCrtState[0] = SceCrtState[0] * 10 + 1;
    for (u64 index = 0; index < sizeof initCallbacks / sizeof initCallbacks[0]; ++index)
        initCallbacks[index]();
    return 0;
}

EXPORT int SceCrtFini(u64 args, const void* argp, void* param) {
    (void)args;
    (void)argp;
    (void)param;
    ++SceCrtState[2];
    SceCrtState[0] = SceCrtState[0] * 10 + 5;
    for (u64 index = sizeof finiCallbacks / sizeof finiCallbacks[0]; index; --index)
        finiCallbacks[index - 1]();
    return 0;
}

EXPORT int SceCrtCheck(void) {
    if (SceCrtState[0] != 123 || SceCrtState[1] != 1 || SceCrtState[2] ||
        SceCrtState[3] != 1 || SceCrtState[4] != 1 || SceCrtState[5] ||
        SceCrtState[6] || SceCrtState[7] != 82 || SceCrtState[8]) return 71;
    ++SceCrtState[8];
    SceCrtState[0] = SceCrtState[0] * 10 + 4;
    SceCrtState[7] = SceCrtState[7] * 7 + 3;
    return 0;
}
