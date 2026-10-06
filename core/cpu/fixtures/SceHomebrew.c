typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
typedef long i64;

extern void* sce_memcpy(void*, const void*, u64) __asm__("Q3VBxCXhUHs");
extern void* sce_memmove(void*, const void*, u64) __asm__("+P6FRGH4LfA");
extern void* sce_memset(void*, int, u64) __asm__("8zTFvBIAIN8");
extern u64 sce_strlen(const char*) __asm__("j4ViWNHEgww");
extern int sce_strcmp(const char*, const char*) __asm__("Ovb2dSJOAuE");
extern void sce_exit(int) __asm__("uMei1W9uyNo") __attribute__((noreturn));
extern int sce_open(const char*, int, unsigned short) __asm__("1G3lF1Gg1k8");
extern i64 sce_read(int, void*, u64) __asm__("Cg4srZ6TKbU");
extern i64 sce_pread(int, void*, u64, i64) __asm__("+r3rMFwItV4");
extern i64 sce_lseek(int, i64, int) __asm__("oib76F-12fk");
extern int sce_close(int) __asm__("UK2Tl2DWUns");
extern int sce_user_initialize(const void*) __asm__("j3YMu1MVNNo");
extern int sce_user_initial(int*) __asm__("CdWp0oHWGr0");
extern int sce_user_login_list(int*) __asm__("fPhymKNvK-A");
extern int sce_user_name(int, char*, u64) __asm__("1xxcMiGu2fo");
extern int sce_system_int(int, int*) __asm__("fZo48un7LK4");
extern int sce_system_string(int, char*, u64) __asm__("SsC-m-S9JTA");
extern int sce_system_hide_splash(void) __asm__("Vo5V8KAwCmk");
extern u64 sce_direct_size(void) __asm__("pO96TwzOm5E");
extern int sce_available_direct(i64, i64, u64, i64*, u64*) __asm__("C0f7TJcbfac");
extern int sce_allocate_direct(i64, i64, u64, u64, int, i64*) __asm__("rTXw65xmLIA");
extern int sce_allocate_main(u64, u64, int, i64*) __asm__("B+vc2AO2Zrc");
extern int sce_map_direct(void**, u64, int, int, i64, u64) __asm__("L-Q3LEjIbgA");
extern int sce_map_flexible(void**, u64, int, int) __asm__("IWIBBdTHit4");
extern int sce_reserve(void**, u64, int, u64) __asm__("7oxv3PPCumo");
extern int sce_mprotect(const void*, u64, int) __asm__("vSMAm3cxYTY");
extern int sce_virtual_query(const void*, int, void*, u64) __asm__("rVjRvHJ0X6c");
extern int sce_munmap(void*, u64) __asm__("cQke9UuBQOk");
extern int sce_release_direct(i64, u64) __asm__("MBuItvba6z8");

static u8 composite[4096];
static u8 pixels[4096];
static u8 copied[4096];
static u8 resource[128];
static u8 positional[7];
static const char* volatile modes[3] = {"callback", "import", "unsupported-flags"};

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

struct QueryInfo {
    u64 Start, End, Offset;
    int Protection, MemoryType;
    u32 Flags;
    char Name[32];
    u8 GpuMask, Reserved, Padding[2];
};
_Static_assert(sizeof(struct QueryInfo) == 72, "PS5 virtual query ABI must be 72 bytes");

static int memory_services(u32 seed, u32* checksum) {
    const u64 page = 0x4000;
    const u64 profile = 12UL * 1024 * 1024 * 1024;
    i64 availableStart = -1, physical = -1, mainPhysical = -1;
    u64 availableSize = 0;
    if (sce_direct_size() != profile ||
        sce_available_direct(0, (i64)profile, 0, &availableStart, &availableSize) != 0 ||
        availableStart != 0 || availableSize != profile) return 115;
    if (sce_allocate_direct((i64)page, (i64)profile, 2 * page, 0x10000, 0, &physical) != 0 ||
        physical != 0x10000) return 116;
    if (sce_allocate_main(page, page, 12, &mainPhysical) != 0 || mainPhysical != 0) return 117;
    void* first = (void*)0x1000000000UL;
    void* second = (void*)0x1000100000UL;
    if (sce_map_direct(&first, 2 * page, 2, 0x90, physical, 0x10000) != 0 ||
        first != (void*)0x1000000000UL ||
        sce_map_direct(&second, 2 * page, 2, 0x90, physical, 0x10000) != 0 ||
        second != (void*)0x1000100000UL) return 118;
    volatile u8* left = (volatile u8*)first + page;
    volatile u8* right = (volatile u8*)second + page;
    for (u64 i = 0; i < 4096; ++i) {
        if (left[i] || right[i]) return 119;
        left[i] = (u8)(((i * 29 + (i >> 2) + seed * 11) ^ (i >> 4)) ^ (i * 7 + seed * 3));
    }
    u32 a = 1, b = 0;
    for (u64 i = 0; i < 4096; ++i) {
        right[i] ^= (u8)(i * 7 + seed * 3);
        const u8 expected = (u8)((i * 29 + (i >> 2) + seed * 11) ^ (i >> 4));
        if (left[i] != expected) return 120;
        a = (a + left[i]) % 65521;
        b = (b + a) % 65521;
    }
    *checksum = (b << 16) | a;
    volatile u8* instructions = (volatile u8*)first;
    instructions[0] = 0xb8; instructions[1] = 0xd2; instructions[2] = 0x04;
    instructions[3] = 0; instructions[4] = 0; instructions[5] = 0xc3;
    if (sce_mprotect(first, page, 5) != 0 || ((int (*)(void))first)() != 1234) return 121;
    struct { u64 Before; struct QueryInfo Info; u64 After; } query;
    sce_memset(&query, 0xa5, sizeof query);
    if (sce_virtual_query(first, 0, &query.Info, sizeof query.Info) != 0 ||
        query.Info.Start != (u64)first || query.Info.End != (u64)first + page ||
        query.Info.Offset != (u64)physical || query.Info.Protection != 5 ||
        query.Info.MemoryType != 0 || query.Info.Flags != 0x12 || query.Info.GpuMask ||
        query.Info.Reserved || query.Info.Padding[0] || query.Info.Padding[1] ||
        query.Before != 0xa5a5a5a5a5a5a5a5UL || query.After != 0xa5a5a5a5a5a5a5a5UL) return 122;
    for (u64 i = 0; i < sizeof query.Info.Name; ++i) if (query.Info.Name[i]) return 122;
    if (sce_virtual_query((const u8*)first + page, 0, &query.Info, sizeof query.Info) != 0 ||
        query.Info.Start != (u64)first + page || query.Info.End != (u64)first + 2 * page ||
        query.Info.Offset != (u64)physical + page || query.Info.Protection != 2 ||
        query.Info.MemoryType != 0 || query.Info.Flags != 0x12) return 123;
    void* flexible = (void*)0x1000200000UL;
    if (sce_map_flexible(&flexible, page, 3, 0x90) != 0 || flexible != (void*)0x1000200000UL) return 124;
    volatile u64* values = (volatile u64*)flexible;
    u64 total = 0;
    for (u64 i = 0; i < 64; ++i) {
        if (values[i]) return 125;
        values[i] = i * i + seed;
        total += values[i];
    }
    if (total != 85344 + 64 * (u64)seed ||
        sce_virtual_query(flexible, 0, &query.Info, sizeof query.Info) != 0 ||
        query.Info.Start != (u64)flexible || query.Info.End != (u64)flexible + page ||
        query.Info.Offset || query.Info.Protection != 3 || query.Info.MemoryType || query.Info.Flags != 0x11) return 125;
    void* reserved = (void*)0x1000300000UL;
    if (sce_reserve(&reserved, page, 0x90, 0x10000) != 0 || reserved != (void*)0x1000300000UL ||
        sce_virtual_query(reserved, 0, &query.Info, sizeof query.Info) != 0 ||
        query.Info.Start != (u64)reserved || query.Info.End != (u64)reserved + page ||
        query.Info.Offset || query.Info.Protection || query.Info.MemoryType || query.Info.Flags) return 126;
    if (sce_munmap(reserved, page) != 0 || sce_munmap(flexible, page) != 0 ||
        sce_munmap(first, 2 * page) != 0 || sce_munmap(second, 2 * page) != 0) return 127;
    if (sce_release_direct(physical, 2 * page) != 0 || sce_release_direct(mainPhysical, page) != 0 ||
        sce_available_direct(0, (i64)profile, 0, &availableStart, &availableSize) != 0 ||
        availableStart != 0 || availableSize != profile || sce_direct_size() != profile) return 128;
    return 0;
}

int SceGuestMain(u64 argc, char** argv, u64 exitCallback, u64 entryAlignment, u64 sharedArgumentBlock) {
    if (entryAlignment != 8 || !exitCallback || !sharedArgumentBlock) return 80;
    if (argc != 10 || !argv[0] || !sce_strlen(argv[0]) || argv[argc]) return 81;
    u32 limit, seed, expectedCount, expectedSum, expectedAdler, expectedSize, expectedResourceAdler, expectedMemoryAdler;
    if (!number(argv[1], &limit) || !number(argv[2], &seed) ||
        !number(argv[3], &expectedCount) || !number(argv[4], &expectedSum) ||
        !number(argv[5], &expectedAdler) || !number(argv[7], &expectedSize) ||
        !number(argv[8], &expectedResourceAdler) || !number(argv[9], &expectedMemoryAdler) || limit < 2 || limit > 4095 || seed > 255 ||
        expectedSize < 10 || expectedSize >= sizeof resource) return 82;
    int importedExit;
    if (sce_strcmp(argv[6], modes[0]) == 0) importedExit = 0;
    else if (sce_strcmp(argv[6], modes[1]) == 0) importedExit = 1;
    else if (sce_strcmp(argv[6], modes[2]) == 0) {
        sce_open("/app0/resource.bin", 2, 0);
        return 103;
    }
    else return 83;
    struct { u32 Before; int Id; u32 After; } user;
    struct { u32 Before; int Ids[4]; u32 After; } users;
    struct { u8 Before; char Bytes[17]; u8 After; } name;
    sce_memset(&user, 0xa5, sizeof user);
    sce_memset(&users, 0xa5, sizeof users);
    sce_memset(&name, 0xa5, sizeof name);
    if (sce_user_initialize((void*)0) != 0) return 104;
    if (sce_user_initial(&user.Id) != 0 || user.Id != 0x10000000) return 105;
    if (sce_user_login_list(users.Ids) != 0 || users.Ids[0] != user.Id ||
        users.Ids[1] != -1 || users.Ids[2] != -1 || users.Ids[3] != -1) return 106;
    if (sce_user_name(user.Id, name.Bytes, sizeof name.Bytes) != 0 || name.Bytes[6] != 0 ||
        sce_strcmp(name.Bytes, "Player") != 0) return 107;
    for (u64 i = 7; i < sizeof name.Bytes; ++i) if (name.Bytes[i]) return 107;
    if (user.Before != 0xa5a5a5a5u || user.After != 0xa5a5a5a5u ||
        users.Before != 0xa5a5a5a5u || users.After != 0xa5a5a5a5u ||
        name.Before != 0xa5 || name.After != 0xa5) return 108;
    struct { u32 Before; int Language; int Zone; int Summer; u32 After; } system;
    struct { u8 Before; char Bytes[65]; u8 After; } systemName;
    sce_memset(&system, 0xa7, sizeof system);
    sce_memset(&systemName, 0xa7, sizeof systemName);
    if (sce_system_int(1, &system.Language) != 0 || system.Language != 1) return 109;
    if (sce_system_int(4, &system.Zone) != 0 || system.Zone != 0) return 110;
    if (sce_system_int(5, &system.Summer) != 0 || system.Summer != 0) return 111;
    if (sce_system_string(6, systemName.Bytes, sizeof systemName.Bytes) != 0 ||
        systemName.Bytes[6] != 0 || sce_strcmp(systemName.Bytes, "AnyPS5") != 0) return 112;
    for (u64 i = 7; i < sizeof systemName.Bytes; ++i)
        if ((u8)systemName.Bytes[i] != 0xa7) return 113;
    if (system.Before != 0xa7a7a7a7u || system.After != 0xa7a7a7a7u ||
        systemName.Before != 0xa7 || systemName.After != 0xa7) return 113;
    if (sce_system_hide_splash() != 0 || sce_system_hide_splash() != 0) return 114;
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
    if (sce_open("/app0/missing.bin", 0, 0) != (int)0x80020002u) return 93;
    const int descriptor = sce_open("/app0/resource.bin", 0, 0);
    if (descriptor < 3) return 94;
    if (sce_read(descriptor, resource, 5) != 5 || sce_lseek(descriptor, 0, 1) != 5) return 95;
    if (sce_pread(descriptor, positional, sizeof positional, 3) != sizeof positional ||
        sce_lseek(descriptor, 0, 1) != 5) return 96;
    if (sce_lseek(descriptor, 0, 0) != 0 || sce_read(descriptor, resource, sizeof resource) != expectedSize) return 97;
    u8 last[2] = {0x51, 0x92};
    if (sce_read(descriptor, last, sizeof last) != 0 || last[0] != 0x51 || last[1] != 0x92) return 98;
    for (u64 i = 0; i < sizeof positional; ++i) if (positional[i] != resource[i + 3]) return 99;
    if (sce_lseek(descriptor, -2, 2) != expectedSize - 2 || sce_read(descriptor, last, sizeof last) != 2 ||
        last[0] != resource[expectedSize - 2] || last[1] != resource[expectedSize - 1]) return 100;
    if (sce_read(descriptor, (void*)0, 1) != (i64)(int)0x8002000eu ||
        sce_lseek(descriptor, 0, 1) != expectedSize) return 101;
    if (sce_close(descriptor) != 0 || sce_close(descriptor) != (int)0x80020009u ||
        sce_pread(descriptor, last, 1, 0) != (i64)(int)0x80020009u) return 102;
    u32 memoryChecksum = 0;
    const int memoryStatus = memory_services(seed, &memoryChecksum);
    if (memoryStatus) return memoryStatus;
    const int status = count == expectedCount && sum == expectedSum && checksum == expectedAdler &&
                       adler32(resource, expectedSize) == expectedResourceAdler && memoryChecksum == expectedMemoryAdler ? 0 : 77;
    if (importedExit) sce_exit(status);
    ((void (*)(void))exitCallback)();
    return 92;
}
