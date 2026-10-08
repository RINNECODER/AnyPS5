/* Genuine linked public x86 imports; literal candidate errors, no provider headers. */
typedef unsigned long long u64;
typedef unsigned int u32;
extern int sceNetSocket(const char*, u64, u64, u64);
extern int sceNetSocketClose(u64);
extern int* sceNetErrnoLoc(void);
__asm__(".type sceNetSocket,@function\n"
        ".type sceNetSocketClose,@function\n"
        ".type sceNetErrnoLoc,@function\n");
static const char name31[] = "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn";
static const char* volatile relocatedName = name31;
typedef int (*Create)(u64*, const u64*, void* (*)(void*), void*, const char*);
typedef int (*Join)(u64, void**);
typedef u64 (*Self)(void);
typedef void (*Observe)(u64);
typedef void (*Exit)(int);
struct Arguments {
    Create create; Join join; Self self; Observe observe; Exit exit;
    volatile u64* receipt; u64 mode;
};
static void* child(void*);

__attribute__((visibility("protected")))
int _start(struct Arguments* a) {
    volatile u64* r = a->receipt;
    r[0] = 0; r[1] = a->self(); r[2] = (u64)sceNetErrnoLoc();
    r[3] = (u64)sceNetErrnoLoc(); a->observe(10);
    if (r[2] != r[3]) { a->exit(1); return 1; }
    *(int*)r[2] = 0x12345;
    /* High register halves are deliberately dirty; ABI arguments are signed32. */
    int tcp = sceNetSocket(relocatedName, 0xabcdef0100000002ULL,
                        0x9876543200000001ULL, 0xaabbccdd00000006ULL);
    int udp = sceNetSocket((const char*)0, 2, 2, 17);
    r[4] = (u32)tcp; r[5] = (u32)udp;
    if (tcp < 0 || udp < 0 || tcp == udp || *(int*)r[2] != 0x12345) { a->exit(2); return 2; }
    a->observe(1);
    r[6] = (u32)sceNetSocketClose(0x7788990000000000ULL | (u32)tcp);
    if (r[6] || *(int*)r[2] != 0x12345) { a->exit(3); return 3; }
    a->observe(2);
    r[7] = (u32)sceNetSocketClose((u32)tcp);
    if (r[7] != 0x80410109U || *(int*)r[2] != 9) { a->exit(4); return 4; }
    if (a->mode) {
        *(int*)r[2] = 0x12345;
        u64 id = 0; void* result = (void*)0;
        if (a->create(&id, (const u64*)0, child, a, (const char*)0)) { a->exit(5); return 5; }
        r[8] = id;
        if (a->join(id, &result) || (u64)result != 0x123456789abcdef0ULL) { a->exit(6); return 6; }
        r[9] = (u64)sceNetErrnoLoc(); r[10] = (u32)*(int*)r[9];
        if (r[9] != r[2] || r[10] != 0x12345 || r[11] == r[1] ||
            r[12] == r[2] || r[12] != r[13] || r[14] != 0x80410109U ||
            r[15] != 9 || r[16] != 0x23456) { a->exit(7); return 7; }
    }
    r[0] = 0x4e4554534f434b54ULL;
    /* UDP remains open for the independent provider-teardown resource oracle. */
    a->exit(0);
    return 8;
}

static void* child(void* opaque) {
    struct Arguments* a = (struct Arguments*)opaque;
    volatile u64* r = a->receipt;
    r[11] = a->self(); r[12] = (u64)sceNetErrnoLoc(); r[13] = (u64)sceNetErrnoLoc();
    a->observe(11);
    r[14] = (u32)sceNetSocketClose(0xffffffffffffffffULL); r[15] = (u32)*(int*)r[12];
    *(int*)r[12] = 0x23456;
    int fd = sceNetSocket((const char*)0, 2, 1, 0);
    if (fd < 0 || sceNetSocketClose((u32)fd)) return (void*)1;
    r[16] = (u32)*(int*)r[12];
    return (void*)0x123456789abcdef0ULL;
}
