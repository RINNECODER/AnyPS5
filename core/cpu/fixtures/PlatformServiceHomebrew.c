typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#define EXPORT __attribute__((visibility("protected")))

#if BUILD_PLATFORM_DEPENDENCY

static volatile u32 anchor = 7;
static volatile u32* volatile relocatedAnchor = &anchor;

EXPORT u32 PlatformServiceGuestMath(u32 value) {
    return value * value + value * 3 + *relocatedAnchor;
}

#else

extern u32 PlatformServiceGuestMath(u32);
extern int sce_np_state(u32, int*) __asm__("eQH7nWPcAgc");
extern u64 sce_common_initialize(void) __asm__("uoUpLGNkygk");
extern u32 sce_htonl(u32) __asm__("9T2pDF2Ryqg");
extern u16 sce_htons(u16) __asm__("iWQWrwiSt8A");
extern int sce_inet_pton(int, const char*, void*) __asm__("8Kcp5d-q1Uo");
extern const char* sce_inet_ntop(int, const void*, char*, u32) __asm__("9vA2aW+CHuA");

static const char* volatile addressText = "203.0.113.7";

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

EXPORT int SceGuestMain(u64 argc, char** argv) {
    if (argc != 3 || !argv[0] || !*argv[0] || argv[argc]) return 81;
    u32 input, expectedMath;
    if (!number(argv[1], &input) || !number(argv[2], &expectedMath) || input > 1000) return 82;

    volatile struct { u32 before; int state; u32 after; } np = {0x13579bdfu, -23, 0x2468ace0u};
    if (sce_np_state(0x10000000u, (int*)&np.state) != 0 || np.state != 1 ||
        np.before != 0x13579bdfu || np.after != 0x2468ace0u) return 83;
    if (sce_common_initialize() != 0) return 91;
    if (sce_common_initialize() != 0xffffffff80b80002ul) return 92;
    if (sce_htonl(0x12345678u) != 0x78563412u || sce_htons(0x1234u) != 0x3412u) return 84;

    volatile struct { u8 before[4]; u8 address[4]; u8 after[4]; } binary;
    for (u32 index = 0; index < 4; ++index) {
        binary.before[index] = 0xa5;
        binary.address[index] = 0x6d;
        binary.after[index] = 0x5a;
    }
    if (sce_inet_pton(2, addressText, (void*)binary.address) != 1) return 85;
    const u8 expectedAddress[4] = {203, 0, 113, 7};
    for (u32 index = 0; index < 4; ++index)
        if (binary.before[index] != 0xa5 || binary.address[index] != expectedAddress[index] ||
            binary.after[index] != 0x5a) return 86;

    volatile struct { u8 before[4]; char text[16]; u8 after[4]; } formatted;
    for (u32 index = 0; index < 4; ++index) {
        formatted.before[index] = 0x3c;
        formatted.after[index] = 0xc3;
    }
    for (u32 index = 0; index < 16; ++index) formatted.text[index] = (char)0xa5;
    if (sce_inet_ntop(2, (const void*)binary.address, (char*)formatted.text, sizeof(formatted.text)) != (const char*)formatted.text) return 87;
    const char expectedText[] = "203.0.113.7";
    for (u32 index = 0; index < sizeof(expectedText); ++index)
        if (formatted.text[index] != expectedText[index]) return 88;
    for (u32 index = sizeof(expectedText); index < sizeof(formatted.text); ++index)
        if ((u8)formatted.text[index] != 0xa5) return 89;
    for (u32 index = 0; index < 4; ++index)
        if (formatted.before[index] != 0x3c || formatted.after[index] != 0xc3) return 90;

    return PlatformServiceGuestMath(input) == expectedMath ? 0 : 77;
}

#endif
