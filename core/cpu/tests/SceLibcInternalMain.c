typedef unsigned long size_t;
#define EXPORT __attribute__((visibility("protected")))
extern void* memcpy(void*, const void*, size_t);
extern void* memset(void*, int, size_t);
extern size_t strlen(const char*);
extern int strcmp(const char*, const char*);
extern int strncmp(const char*, const char*, size_t);
extern char* strncpy(char*, const char*, size_t);
extern int snprintf(char*, size_t, const char*, ...);
extern int printf(const char*, ...);
extern char* strstr(const char*, const char*);
extern volatile size_t SceInternalState[11];
extern unsigned char SceInternalConsumerState[8];
extern void* sceLibcMspaceCreate(const char*, void*, size_t, int);
extern int sceLibcMspaceDestroy(void*);
extern void* sceLibcMspaceMalloc(void*, size_t);
extern int sceLibcMspaceFree(void*, void*);
extern void* sceLibcMspaceRealloc(void*, void*, size_t);
struct Stats { unsigned short size, version; unsigned reserved;
    size_t max_system, current_system, max_inuse, current_inuse; };
extern int sceLibcMspaceMallocStats(void*, struct Stats*);
extern int __cxa_atexit(void (*)(void*), void*, void*);
extern void __cxa_finalize(void*);
extern void* _Znwm(size_t);
extern void _ZdlPv(void*);
extern char* strncat(char*, const char*, size_t);
extern size_t _Stoul(const char*, char**, int);
extern int* guest_error(void) __asm__("9BcDykPmo1I");
extern void abort(void);
extern void __cxa_pure_virtual(void);
EXPORT void* SceInternalAddresses[] = {memcpy, memset, strlen, strcmp, strncmp, strncpy, snprintf, printf, strstr,
    sceLibcMspaceCreate, sceLibcMspaceDestroy, sceLibcMspaceMalloc, sceLibcMspaceFree, sceLibcMspaceRealloc,
    sceLibcMspaceMallocStats, __cxa_atexit, __cxa_finalize, _ZdlPv, strncat, _Stoul, abort, __cxa_pure_virtual};
EXPORT size_t SceInternalExtraOutput[26];
EXPORT unsigned char SceInternalArenas[2][4096] __attribute__((aligned(16)));
struct GuardStats { size_t before; struct Stats value; size_t after; };
EXPORT struct GuardStats SceInternalStats[3];
EXPORT size_t SceInternalTrace[12];
static unsigned trace_size;
static unsigned dso_a, dso_b;
struct Object { size_t id; void* dso; int reenter; };
static struct Object objects[] = {{11, &dso_a, 0}, {22, &dso_b, 0}, {33, &dso_a, 1}, {44, &dso_a, 0}, {55, &dso_a, 0}, {66, &dso_a, 0}, {77, &dso_a, 0}};
static void callback(void* pointer) {
    struct Object* o = pointer; SceInternalTrace[trace_size++] = o->id;
    if (o->reenter) { __cxa_finalize(o->dso); __cxa_atexit(callback, &objects[3], o->dso); }
}
static int extended(void);
EXPORT unsigned char SceInternalOutput[160];

EXPORT int SceInternalMain(void) {
    if (SceInternalState[0] != 1 || SceInternalState[1]) return 31;
    for (unsigned i = 0; i < 8; ++i) if (SceInternalConsumerState[i] != "orderok"[i]) return 43;
    for (unsigned i = 0; i < sizeof SceInternalOutput; ++i) SceInternalOutput[i] = 0xa7;
    if (memcpy(SceInternalOutput + 1, "native", 6) != SceInternalOutput + 1) return 32;
    if (memset(SceInternalOutput + 8, 0x134, 3) != SceInternalOutput + 8) return 33;
    if (strlen("guest") != 5 || strcmp("ab", "ac") >= 0 || strcmp("ac", "ab") <= 0) return 34;
    if (strncmp("abc", "abd", 2) || strncmp("abc", "abd", 3) >= 0 || strncmp("z", "a", 0)) return 35;
    if (strncpy((char*)SceInternalOutput + 16, "xy", 5) != (char*)SceInternalOutput + 16) return 36;
    if (strncpy((char*)SceInternalOutput + 24, "longer", 3) != (char*)SceInternalOutput + 24) return 37;
    const char* format = "%s:%u,%u,%u,%u,%u,%u,%u:%.1f";
    int size = snprintf((char*)SceInternalOutput + 40, 64, format, "guest", 11u, 22u, 33u, 44u, 55u, 66u, 77u, 3.5);
    if (size != 30) return 38;
    if (snprintf((char*)SceInternalOutput + 112, 5, format, "guest", 11u, 22u, 33u, 44u, 55u, 66u, 77u, 3.5) != size) return 39;
    if (snprintf((void*)0, 0, "%s:%u", "count", 19u) != 8) return 40;
    if (printf("print:%u", 91u) != 8) return 41;
    const char* text = "ababa!";
    if (strstr(text, "aba") != text || strstr(text, "ba!") != text + 3 || strstr(text, "zzz") || strstr(text, "") != text) return 42;
    return extended();
}

static int extended(void) {
    for (unsigned i = 0; i < sizeof SceInternalStats; ++i) ((unsigned char*)SceInternalStats)[i] = 0xa7;
    for (unsigned i = 0; i < 3; ++i) { SceInternalStats[i].value.size = i == 2 ? 39 : 40; SceInternalStats[i].value.version = 1; }
    void* a = sceLibcMspaceCreate("heap-a", SceInternalArenas[0], 4096, 0);
    void* b = sceLibcMspaceCreate("heap-b", SceInternalArenas[1], 4096, 1);
    SceInternalExtraOutput[0] = (size_t)a; SceInternalExtraOutput[1] = (size_t)b;
    if (!a || !b || a == b) return 51;
    SceInternalExtraOutput[3] = (size_t)sceLibcMspaceMalloc(a, (1UL << 32) | 8);
    unsigned char* p = sceLibcMspaceMalloc(a, 8);
    unsigned char* q = sceLibcMspaceMalloc(b, 8);
    if (!p || !q || p == q) return 52;
    for (unsigned i = 0; i < 8; ++i) { p[i] = (unsigned char)(0xb0 + i); q[i] = (unsigned char)(0xc0 + i); }
    unsigned char* moved = sceLibcMspaceRealloc(a, p, 32);
    SceInternalExtraOutput[2] = (size_t)moved;
    if (!moved || moved == p) return 53;
    for (unsigned i = 0; i < 8; ++i) if (moved[i] != 0xb0 + i || q[i] != 0xc0 + i) return 54;
    SceInternalExtraOutput[4] = (size_t)sceLibcMspaceRealloc(a, moved, (1UL << 32) | 32);
    SceInternalExtraOutput[5] = sceLibcMspaceMallocStats(a, &SceInternalStats[0].value);
    SceInternalExtraOutput[6] = sceLibcMspaceMallocStats(a, &SceInternalStats[2].value);
    SceInternalExtraOutput[7] = sceLibcMspaceFree(a, moved);
    SceInternalExtraOutput[8] = sceLibcMspaceFree((void*)0, (void*)0);
    SceInternalExtraOutput[9] = sceLibcMspaceMallocStats(a, &SceInternalStats[1].value);
    SceInternalExtraOutput[10] = sceLibcMspaceFree(b, q);
    SceInternalExtraOutput[11] = sceLibcMspaceDestroy(a);
    SceInternalExtraOutput[12] = sceLibcMspaceDestroy(b);
    /* Producer remains an ordinary libc-scope guest export, paired with delete. */
    unsigned char* object = _Znwm(8); if (!object) return 55;
    object[0] = 0x9b; _ZdlPv(object); _ZdlPv((void*)0);
    if (__cxa_atexit(callback, &objects[0], &dso_a) || __cxa_atexit(callback, &objects[1], &dso_b) ||
        __cxa_atexit(callback, &objects[2], &dso_a)) return 56;
    __cxa_finalize(&dso_a); __cxa_finalize(&dso_a); __cxa_finalize(&dso_a); __cxa_finalize(&dso_b);
    if (__cxa_atexit(callback, &objects[4], &dso_a)) return 57;
    /* The 8-record model makes allocation failure deterministic, leaving live
       guest records for CRT cleanup; no retail capacity claim is made. */
    if (__cxa_atexit(callback, &objects[5], &dso_a) || __cxa_atexit(callback, &objects[6], &dso_a)) return 58;
    SceInternalExtraOutput[13] = (size_t)(long)__cxa_atexit(callback, &objects[4], &dso_a);
    char* text = (char*)SceInternalOutput + 128; text[0] = 'p'; text[1] = 0;
    if (strncat(text, "abcd", (1UL << 32) | 2) != text || strncat(text, "zz", 0) != text) return 59;
    text = (char*)SceInternalOutput + 144; text[0] = 'q'; text[1] = 0;
    if (strncat(text, "wxyz", 2) != text) return 60;
    struct End { size_t before; char* pointer; size_t after; } end = {0x1122334455667788UL, 0, 0x8877665544332211UL};
    const char* number = "4294967297tail";
    SceInternalExtraOutput[14] = _Stoul(number, &end.pointer, 10);
    SceInternalExtraOutput[15] = end.pointer - number;
    SceInternalExtraOutput[25] = (unsigned)*guest_error();
    number = "-2!"; SceInternalExtraOutput[16] = _Stoul(number, &end.pointer, 10); SceInternalExtraOutput[17] = end.pointer - number;
    number = "184467440737095516160tail"; SceInternalExtraOutput[18] = _Stoul(number, &end.pointer, 10); SceInternalExtraOutput[19] = end.pointer - number;
    number = "0x"; SceInternalExtraOutput[20] = _Stoul(number, &end.pointer, 0); SceInternalExtraOutput[21] = end.pointer - number;
    number = "123";
    int bases[] = {-1, 1, 37};
    for (unsigned i = 0; i < 3; ++i) if (_Stoul(number, &end.pointer, bases[i]) || end.pointer != number) return 61;
    SceInternalExtraOutput[22] = _Stoul("0x100000001", (void*)0, 0);
    SceInternalExtraOutput[23] = end.before; SceInternalExtraOutput[24] = end.after;
    return 0;
}
/* Deliberately callable probes allow the host oracle to examine no-return
   behavior without terminating the successful lifecycle/data case. */
EXPORT void SceInternalAbortProbe(void) { SceInternalExtraOutput[0] = 1; abort(); SceInternalExtraOutput[0] = 9; }
EXPORT void SceInternalPureProbe(void) { SceInternalExtraOutput[0] = 1; __cxa_pure_virtual(); SceInternalExtraOutput[0] = 9; }
EXPORT void SceInternalOwnershipProbe(void) {
    void* a = sceLibcMspaceCreate("heap-a", SceInternalArenas[0], 4096, 0);
    void* b = sceLibcMspaceCreate("heap-b", SceInternalArenas[1], 4096, 0);
    void* p = sceLibcMspaceMalloc(a, 8); sceLibcMspaceFree(b, p); SceInternalExtraOutput[0] = 9;
}
