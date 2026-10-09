typedef unsigned long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))

#if BUILD_LAZY_DEPENDENCY

static volatile u32 anchor = 7;
static volatile u32* volatile relocatedAnchor = &anchor;

EXPORT u32 LazyImportGuestValue(void) {
    return *relocatedAnchor;
}

#else

extern u32 LazyImportGuestValue(void);
// libkernel NID that no host provider implements; imported but never called.
extern int anyps5LazyNeverCalled(void);
// Module that no guest image or host provider supplies.
extern u64 anyps5LazyAbsentCall(u64);
extern int anyps5LazyAbsentWeak(void) __attribute__((weak));
// Offline NP / NpWebApi / NpWebApi2 providers.
extern int sceNpWebApi2Initialize(int, u64);
extern int sceNpWebApi2CreateUserContext(int, int);
extern int sceNpWebApi2CreateRequest(int, const char*, const char*, const char*, const void*, long*);
extern int sceNpWebApi2SendRequest(long, const void*, u64, void*);
extern int sceNpWebApi2DeleteRequest(long);
extern int sceNpWebApi2DeleteUserContext(int);
extern int sceNpWebApi2Terminate(int);
extern int sceNpGetAccountIdA(int, u64*);
extern int sceNpGetNpReachabilityState(int, u32*);
extern int sceNpWebApiInitialize(int, u64);
extern int sceNpWebApiTerminate(int);

static int (*volatile neverCalled)(void) = anyps5LazyNeverCalled;
static const u32 sessionUser = 0x10000000u;
static const u32* volatile relocatedUser = &sessionUser;

static int number(const char* text, u64* result) {
    if (!text || !*text) return 0;
    u64 value = 0, length = 0;
    while (*text) {
        if (++length > 10 || *text < '0' || *text > '9') return 0;
        value = value * 10 + (unsigned)(*text++ - '0');
    }
    *result = value;
    return 1;
}

EXPORT int SceGuestMain(u64 argc, char** argv) {
    u64 expected;
    if (argc != 2 || !argv[0] || !*argv[0] || argv[argc] || !number(argv[1], &expected)) return 81;
    if (LazyImportGuestValue() != 7 || !neverCalled) return 82;
    const u32 user = *relocatedUser;

    const int library = sceNpWebApi2Initialize(1, 0x10000);
    if (library <= 0) return 70;
    const int context = sceNpWebApi2CreateUserContext(library, (int)user);
    if (context <= 0) return 71;
    volatile long request = -1;
    if (sceNpWebApi2CreateRequest(context, "userProfile", "/v1/users/me", "GET", 0, (long*)&request) != 0 ||
        request <= 0) return 72;
    if (sceNpWebApi2SendRequest(request, 0, 0, 0) >= 0) return 73;
    if (sceNpWebApi2DeleteRequest(request) || sceNpWebApi2DeleteUserContext(context) ||
        sceNpWebApi2Terminate(library)) return 74;

    volatile u64 account = 0x5a5a5a5a5a5a5a5aul;
    if (sceNpGetAccountIdA((int)user, (u64*)&account) != (int)0x80550006u ||
        account != 0x5a5a5a5a5a5a5a5aul) return 75;
    volatile u32 reachability = 7;
    if (sceNpGetNpReachabilityState((int)user, (u32*)&reachability) != 0 || reachability != 0) return 76;
    const int webApi = sceNpWebApiInitialize(1, 0x10000);
    if (webApi <= 0 || sceNpWebApiTerminate(webApi)) return 78;

    if (anyps5LazyAbsentWeak) return 79;
    return anyps5LazyAbsentCall(expected) == expected ? 0 : 80;
}

#endif
