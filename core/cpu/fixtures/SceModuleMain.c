typedef unsigned long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))

extern volatile u64 SceModuleObject, SceModuleInitCount, SceModuleFiniCount, SceModuleOrder;
extern __thread volatile u64 SceModuleTls __attribute__((tls_model("initial-exec")));
extern __thread volatile u64 SceModuleTlsZero __attribute__((tls_model("initial-exec")));
extern int SceModuleMath(u32, u32, u32*, u32*, u32*);
extern const u64* SceModuleImportedTlsIndex(void);
extern const u64* SceModuleMainTlsIndex(void);

EXPORT volatile u64 SceModuleMainInitCount;
EXPORT volatile u64 SceModuleMainState[11];
EXPORT volatile u64* volatile SceModuleObjectAddress = &SceModuleObject;
static u64 anchor = 0x69887796ul;
EXPORT __thread volatile u64 SceModuleMainTls __attribute__((aligned(16), tls_model("initial-exec"))) = 0x31415926ul;
EXPORT __thread u64* volatile SceModuleMainTlsPointer __attribute__((tls_model("initial-exec"))) = &anchor;
EXPORT __thread volatile u64 SceModuleMainTlsZero __attribute__((tls_model("initial-exec")));
static u64* volatile relocatedAnchor = &anchor;

EXPORT int SceModuleMainInit(u64 args, const void* argp, void* param) {
    (void)args;
    (void)argp;
    (void)param;
    ++SceModuleMainInitCount;
    ++SceModuleMainTls;
    SceModuleOrder = SceModuleOrder * 10 + 2;
    return 0;
}

static int number(const char* text, u32* result) {
    if (!text || !*text) return 0;
    u64 value = 0, length = 0;
    while (*text) {
        if (++length > 10 || *text < '0' || *text > '9') return 0;
        value = value * 10 + (unsigned)(*text++ - '0');
        if (value > 0xffffffffu) return 0;
    }
    *result = (u32)value;
    return 1;
}

EXPORT int SceModuleMain(u64 argc, char** argv) {
    if (argc != 6 || !argv[0] || !*argv[0] || argv[argc]) return 81;
    u32 limit, seed, expectedCount, expectedSum, expectedAdler;
    if (!number(argv[1], &limit) || !number(argv[2], &seed) ||
        !number(argv[3], &expectedCount) || !number(argv[4], &expectedSum) ||
        !number(argv[5], &expectedAdler) || limit < 2 || limit > 4095 || seed > 255) return 82;
    const u64* importedTlsIndex = SceModuleImportedTlsIndex();
    const u64* mainTlsIndex = SceModuleMainTlsIndex();
    if (SceModuleInitCount != 1 || SceModuleMainInitCount != 1 || SceModuleFiniCount ||
        SceModuleOrder != 12 || SceModuleMainTls != 0x31415927ul || SceModuleMainTlsZero ||
        SceModuleTls != 0x22334456ul || SceModuleTlsZero ||
        SceModuleObject != 0x10203041ul || SceModuleObjectAddress != &SceModuleObject ||
        *relocatedAnchor != 0x69887796ul || SceModuleMainTlsPointer != &anchor ||
        *SceModuleMainTlsPointer != 0x69887796ul || importedTlsIndex[0] != 2 ||
        importedTlsIndex[1] != 0 || mainTlsIndex[0] != 1 || mainTlsIndex[1] != 0) return 83;
    *SceModuleObjectAddress += limit;
    u32 count = 0, sum = 0, checksum = 0;
    const int status = SceModuleMath(limit, seed, &count, &sum, &checksum);
    if (status) return status;
    if (SceModuleTls != 0x22334456ul + seed || SceModuleTlsZero != (u64)sum + seed) return 84;
    SceModuleTls += 7;
    SceModuleMainTls += limit;
    SceModuleMainTlsZero = (u64)count + seed;
    SceModuleMainState[0] = SceModuleInitCount;
    SceModuleMainState[1] = SceModuleMainInitCount;
    SceModuleMainState[2] = SceModuleOrder;
    SceModuleMainState[3] = count;
    SceModuleMainState[4] = sum;
    SceModuleMainState[5] = checksum;
    SceModuleMainState[6] = SceModuleTls;
    SceModuleMainState[7] = SceModuleMainTls;
    SceModuleMainState[8] = SceModuleObject;
    SceModuleMainState[9] = SceModuleTlsZero;
    SceModuleMainState[10] = SceModuleMainTlsZero;
    return count == expectedCount && sum == expectedSum && checksum == expectedAdler ? 0 : 77;
}
