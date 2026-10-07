typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
#define EXPORT __attribute__((visibility("protected")))

EXPORT volatile u64 SceModuleObject = 0x10203040ul;
EXPORT volatile u64 SceModuleInitCount;
EXPORT volatile u64 SceModuleFiniCount;
EXPORT volatile u64 SceModuleOrder;
static u64 anchor = 0x59687786ul;
EXPORT __thread volatile u64 SceModuleTls __attribute__((aligned(16), tls_model("initial-exec"))) = 0x22334455ul;
EXPORT __thread u64* volatile SceModuleTlsPointer __attribute__((tls_model("initial-exec"))) = &anchor;
EXPORT __thread volatile u64 SceModuleTlsZero __attribute__((tls_model("initial-exec")));
extern const u64* SceModuleTlsIndex(void);
extern u64 strlen(const char*);

static u64* volatile relocatedAnchor = &anchor;
static u8 composite[4096];

EXPORT int SceModuleInit(u64 args, const void* argp, void* param) {
    (void)args;
    (void)argp;
    (void)param;
    ++SceModuleInitCount;
    ++SceModuleObject;
    ++SceModuleTls;
    SceModuleOrder = SceModuleOrder * 10 + 1;
    return 0;
}

EXPORT int SceModuleFini(u64 args, const void* argp, void* param) {
    (void)args;
    (void)argp;
    (void)param;
    ++SceModuleFiniCount;
    SceModuleObject += 3;
    SceModuleTls += 5;
    SceModuleOrder = SceModuleOrder * 10 + 3;
    return 0;
}

EXPORT int SceModuleMath(u32 limit, u32 seed, u32* count, u32* sum, u32* checksum) {
    if (strlen("SceModuleGuest") != 14) return 70;
    const u64* tlsIndex = SceModuleTlsIndex();
    if (SceModuleInitCount != 1 || SceModuleFiniCount || SceModuleOrder != 12 ||
        SceModuleObject != 0x10203041ul + limit || SceModuleTls != 0x22334456ul ||
        SceModuleTlsZero || *relocatedAnchor != 0x59687786ul ||
        SceModuleTlsPointer != &anchor || *SceModuleTlsPointer != 0x59687786ul ||
        tlsIndex[0] != 2 || tlsIndex[1] != 0) return 71;
    u32 found = 0, total = 0;
    for (u32 i = 2; i <= limit; ++i) {
        if (composite[i]) continue;
        ++found;
        total += i;
        for (u32 j = i * i; j <= limit; j += i) composite[j] = 1;
    }
    u32 a = 1, b = 0;
    for (u32 i = 0; i < 4096; ++i) {
        const u8 pixel = (u8)((i * 37 + (i >> 3) + seed * 17) ^ (i >> 5));
        a = (a + pixel) % 65521;
        b = (b + a) % 65521;
    }
    SceModuleTls += seed;
    SceModuleTlsZero = (u64)total + seed;
    *count = found;
    *sum = total;
    *checksum = (b << 16) | a;
    return 0;
}
