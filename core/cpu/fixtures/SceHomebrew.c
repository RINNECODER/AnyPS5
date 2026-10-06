typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

extern void* sce_memcpy(void*, const void*, u64) __asm__("Q3VBxCXhUHs");
extern void* sce_memmove(void*, const void*, u64) __asm__("+P6FRGH4LfA");
extern void* sce_memset(void*, int, u64) __asm__("8zTFvBIAIN8");
extern u64 sce_strlen(const char*) __asm__("j4ViWNHEgww");
extern int sce_strcmp(const char*, const char*) __asm__("Ovb2dSJOAuE");
extern void sce_exit(int) __asm__("uMei1W9uyNo") __attribute__((noreturn));

static u8 composite[4096];
static u8 pixels[4096];
static u8 copied[4096];
static const char* volatile modes[2] = {"callback", "import"};

static int number(const char* text, u32* result) {
    u64 length = sce_strlen(text);
    if (!length || length > 10) return 0;
    u64 value = 0;
    for (u64 i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return 0;
        value = value * 10 + (unsigned)(text[i] - '0');
        if (value > 0xffffffffu) return 0;
    }
    *result = (u32)value;
    return 1;
}

static u32 adler32(const u8* bytes, u64 size) {
    u32 a = 1, b = 0;
    for (u64 i = 0; i < size; ++i) {
        a = (a + bytes[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

int SceGuestMain(u64 argc, char** argv, u64 exitCallback, u64 entryAlignment, u64 sharedArgumentBlock) {
    if (entryAlignment != 8 || !exitCallback || !sharedArgumentBlock) return 80;
    if (argc != 7 || !argv[0] || !sce_strlen(argv[0]) || argv[argc]) return 81;
    u32 limit, seed, expectedCount, expectedSum, expectedAdler;
    if (!number(argv[1], &limit) || !number(argv[2], &seed) ||
        !number(argv[3], &expectedCount) || !number(argv[4], &expectedSum) ||
        !number(argv[5], &expectedAdler) || limit < 2 || limit > 4095 || seed > 255) return 82;
    int importedExit;
    if (sce_strcmp(argv[6], modes[0]) == 0) importedExit = 0;
    else if (sce_strcmp(argv[6], modes[1]) == 0) importedExit = 1;
    else return 83;
    for (u64 i = 0; i < sizeof composite; ++i)
        if (composite[i] || pixels[i] || copied[i]) return 84;
    if (sce_memset(composite, 0x177, sizeof composite) != composite) return 85;
    if (composite[0] != 0x77 || composite[2048] != 0x77 || composite[4095] != 0x77) return 86;
    if (sce_memset(composite, 0, sizeof composite) != composite) return 87;
    u32 count = 0, sum = 0;
    for (u64 i = 2; i <= limit; ++i) {
        if (composite[i]) continue;
        ++count;
        sum += (u32)i;
        for (u64 j = i * i; j <= limit; j += i) composite[j] = 1;
    }
    for (u64 i = 0; i < sizeof pixels; ++i)
        pixels[i] = (u8)((i * 37 + (i >> 3) + seed * 17) ^ (i >> 5));
    if (sce_memcpy(copied, pixels, sizeof pixels) != copied) return 88;
    for (u64 i = 0; i < sizeof pixels; ++i) if (copied[i] != pixels[i]) return 89;
    const u32 checksum = adler32(copied, sizeof copied);
    if (sce_memmove(copied + 3, copied, sizeof copied - 3) != copied + 3) return 90;
    for (u64 i = 0; i < sizeof copied; ++i)
        if (copied[i] != pixels[i < 3 ? i : i - 3]) return 91;
    const int status = count == expectedCount && sum == expectedSum && checksum == expectedAdler ? 0 : 77;
    if (importedExit) sce_exit(status);
    ((void (*)(void))exitCallback)();
    return 92;
}
