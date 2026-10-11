typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
#define EXPORT __attribute__((visibility("protected")))

#if BUILD_BRIDGE_DEPENDENCY

static volatile u32 anchor = 7;
static volatile u32* volatile relocatedAnchor = &anchor;

EXPORT u32 UpstreamNpBridgeGuestValue(void) {
    return *relocatedAnchor;
}

#else

// Calls the NP family through the production resolver chain. Every expected value
// comes from upstream core/libs/prx/libSceNp*/Export.cpp, not from the fork's
// hand-written providers.
extern u32 UpstreamNpBridgeGuestValue(void);
extern int sceNpCreateRequest(void);
extern int sceNpDeleteRequest(int);
extern int sceNpSetTimeout(int, int, u32, u32, u32, u32);
extern int sceNpCheckNpAvailability(int, const void*);
extern int sceNpGetState(int, u32*);
extern int sceNpHasSignedUp(int, u8*);
extern int sceNpPollAsync(int, int*);
extern int sceNpGetAccountIdA(int, u64*);
extern int sceNpWebApi2Initialize(int, u64);
extern int sceNpWebApi2CreateUserContext(int, int);
extern int sceNpWebApi2CreateRequest(int, const char*, const char*, const char*, const void*, long*);
extern int sceNpWebApi2SendRequest(long, const void*, u64, void*);
extern void sceNpWebApi2CheckTimeout(void);
extern int sceNpAuthCreateAsyncRequest(const void*);
extern int sceNpAuthPollAsync(int, int*);
extern int sceNpAuthGetAuthorizationCodeV3(int, const void*, void*, int*);
extern int sceNpEntitlementAccessInitialize(const void*, void*);
extern int sceNpEntitlementAccessGetSkuFlag(u32*);
extern int sceNpEntitlementAccessGenerateTransactionId(void*);

struct Label { char data[17]; char padding[3]; };
struct Addcont { struct Label label; u32 package_type; u32 download_status; };
extern int sceNpEntitlementAccessGetAddcontEntitlementInfo(u32, const struct Label*, struct Addcont*);
extern int sceNpEntitlementAccessGetAddcontEntitlementInfoList(u32, struct Addcont*, u32, u32*);

#define NP_INVALID_ARGUMENT ((int)0x80550003u)
#define NP_SIGNED_OUT ((int)0x80550006u)
#define NP_USER_NOT_FOUND ((int)0x80550007u)
#define NP_REQUEST_NOT_FOUND ((int)0x80550014u)
#define WEBAPI2_INVALID_ARGUMENT ((int)0x80553402u)
#define WEBAPI2_UNAVAILABLE ((int)0x80553406u)
#define ENTITLEMENT_PARAMETER ((int)0x80558003u)
#define ENTITLEMENT_NOT_FOUND ((int)0x80558007u)

static const u32 otherUser = 0x10000017u;
static const u32* volatile relocatedUser = &otherUser;

static void fill(volatile void* target, u64 size) {
    for (u64 index = 0; index < size; ++index) ((volatile u8*)target)[index] = 0xa7;
}

static int untouched(const volatile void* target, u64 size) {
    for (u64 index = 0; index < size; ++index)
        if (((const volatile u8*)target)[index] != 0xa7) return 0;
    return 1;
}

static int same(const volatile char* left, const char* right) {
    while (*right) if (*left++ != *right++) return 0;
    return *left == 0;
}

static int manager(void) {
    const int user = (int)*relocatedUser;
    // libSceNpManager keeps its own request counter and live-request set.
    const int request = sceNpCreateRequest();
    if (request != 1) return 10;
    if (sceNpSetTimeout(request, 0, 0, 10000000, 0, 0) != 0) return 11;
    if (sceNpSetTimeout(request + 100, 0, 0, 10000000, 0, 0) != NP_REQUEST_NOT_FOUND) return 12;
    if (sceNpSetTimeout(request, 0, 0, 5, 0, 0) != NP_INVALID_ARGUMENT) return 13;
    volatile struct Label online;
    fill(&online, sizeof online);
    if (sceNpCheckNpAvailability(request, (const void*)&online) != NP_USER_NOT_FOUND) return 14;
    if (sceNpCheckNpAvailability(request, 0) != NP_INVALID_ARGUMENT) return 15;
    if (!untouched(&online, sizeof online)) return 16;
    // Any user reads the signed-out state (1) as four little-endian bytes.
    volatile u8 state[8];
    fill(state, sizeof state);
    if (sceNpGetState(user, (u32*)&state[2]) != 0) return 17;
    if (state[1] != 0xa7 || state[2] != 1 || state[3] || state[4] || state[5] || state[6] != 0xa7) return 18;
    if (sceNpGetState(user, 0) != NP_INVALID_ARGUMENT) return 19;
    volatile u8 signedUp[2] = {0xa5, 0xa7};
    if (sceNpHasSignedUp(user, (u8*)signedUp) != 0 || signedUp[0] != 0 || signedUp[1] != 0xa7) return 20;
    volatile int polled = 7;
    if (sceNpPollAsync(request, (int*)&polled) != 0 || polled != NP_SIGNED_OUT) return 21;
    if (sceNpPollAsync(request, 0) != 0) return 22;
    volatile u64 account = 0x5a5a5a5a5a5a5a5aul;
    if (sceNpGetAccountIdA(user, (u64*)&account) != NP_SIGNED_OUT || account != 0x5a5a5a5a5a5a5a5aul) return 23;
    if (sceNpDeleteRequest(request) != 0) return 24;
    if (sceNpSetTimeout(request, 0, 0, 10000000, 0, 0) != NP_REQUEST_NOT_FOUND) return 25;
    return 0;
}

static int webApi2(void) {
    // libSceNpWebApi2 hands out handles from its own counter, independent of NpManager.
    const int library = sceNpWebApi2Initialize(1, 0x10000);
    if (library != 1) return 30;
    const int context = sceNpWebApi2CreateUserContext(library, (int)*relocatedUser);
    if (context != 2) return 31;
    volatile long request[2] = {-1, -1};
    if (sceNpWebApi2CreateRequest(context, "userProfile", "/v1/users/me", "GET", 0, (long*)&request[0]) != 0) return 32;
    if (request[0] != 3 || request[1] != -1) return 33;
    if (sceNpWebApi2CreateRequest(context, "userProfile", "/v1/users/me", "GET", 0, 0) != WEBAPI2_INVALID_ARGUMENT) return 34;
    if (sceNpWebApi2SendRequest(request[0], 0, 0, 0) != WEBAPI2_UNAVAILABLE) return 35;
    sceNpWebApi2CheckTimeout();
    return 0;
}

static int auth(void) {
    const int request = sceNpAuthCreateAsyncRequest(0);
    if (request != 1) return 40;
    volatile int result = 7;
    if (sceNpAuthPollAsync(request, (int*)&result) != 0 || result != NP_SIGNED_OUT) return 41;
    volatile u8 code[16];
    volatile int issuer = 7;
    if (sceNpAuthGetAuthorizationCodeV3(request, 0, (void*)code, (int*)&issuer) != NP_INVALID_ARGUMENT) return 42;
    if (sceNpAuthGetAuthorizationCodeV3(request, (const void*)code, (void*)code, (int*)&issuer) != NP_SIGNED_OUT ||
        issuer != 7) return 43;
    return 0;
}

static int entitlements(u64 owned) {
    if (sceNpEntitlementAccessInitialize(0, 0) != 0) return 50;
    volatile u32 sku[2] = {0xa7a7a7a7u, 0xa7a7a7a7u};
    if (sceNpEntitlementAccessGetSkuFlag((u32*)&sku[0]) != 0 || sku[0] != 3 || sku[1] != 0xa7a7a7a7u) return 51;
    if (sceNpEntitlementAccessGetSkuFlag(0) != ENTITLEMENT_PARAMETER) return 52;
    volatile u8 transaction[32];
    if (sceNpEntitlementAccessGenerateTransactionId((void*)transaction) != NP_SIGNED_OUT) return 53;

    volatile struct Addcont list[3];
    fill(list, sizeof list);
    volatile u32 hits = 99;
    // Room for one entry: the count reports every owned entitlement, the copy stops at the room.
    if (sceNpEntitlementAccessGetAddcontEntitlementInfoList(0, (struct Addcont*)list, 1, (u32*)&hits) != 0) return 54;
    if (hits != owned) return 55;
    if (owned) {
        if (!same(list[0].label.data, "ANYPS5DLCA") || list[0].package_type != 3 || list[0].download_status != 4) return 56;
        if (!untouched(&list[1], sizeof list[1] * 2)) return 57;
    } else if (!untouched(list, sizeof list)) return 58;
    if (sceNpEntitlementAccessGetAddcontEntitlementInfoList(0, 0, 1, (u32*)&hits) != ENTITLEMENT_PARAMETER) return 59;
    if (sceNpEntitlementAccessGetAddcontEntitlementInfoList(0, 0, 0, (u32*)&hits) != 0 || hits != owned) return 60;

    volatile struct Label wanted;
    fill(&wanted, sizeof wanted);
    const char* name = "ANYPS5DLCB";
    unsigned length = 0;
    while (name[length]) { wanted.data[length] = name[length]; ++length; }
    wanted.data[length] = 0;
    volatile struct Addcont info;
    fill(&info, sizeof info);
    const int found = sceNpEntitlementAccessGetAddcontEntitlementInfo(0, (const struct Label*)&wanted, (struct Addcont*)&info);
    if (owned) {
        if (found != 0 || !same(info.label.data, "ANYPS5DLCB") || info.package_type != 3 || info.download_status != 4) return 61;
    } else if (found != ENTITLEMENT_NOT_FOUND || !untouched(&info, sizeof info)) return 62;
    if (sceNpEntitlementAccessGetAddcontEntitlementInfo(0, 0, (struct Addcont*)&info) != ENTITLEMENT_PARAMETER) return 63;
    return 0;
}

EXPORT int SceGuestMain(u64 argc, char** argv) {
    if (argc != 2 || !argv[1] || (argv[1][0] != '0' && argv[1][0] != '2') || argv[1][1]) return 81;
    if (UpstreamNpBridgeGuestValue() != 7) return 82;
    int status = manager();
    if (!status) status = webApi2();
    if (!status) status = auth();
    if (!status) status = entitlements((u64)(argv[1][0] - '0'));
    return status;
}

#endif
