typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef long i64;
#define EXPORT __attribute__((visibility("protected")))

#if BUILD_BRIDGE_DEPENDENCY

static volatile u32 anchor = 11;
static volatile u32* volatile relocatedAnchor = &anchor;

EXPORT u32 LeafSystemLibsGuestValue(void) {
    return *relocatedAnchor;
}

#else

// Calls the system, dialog, input and NP leaf libraries through the production resolver
// chain. Expected values come from upstream core/libs/prx (bridged exports) or from the
// upstream bodies SceLeafSystemImports stands in for, never from the host implementation.
extern u32 LeafSystemLibsGuestValue(void);

extern int sceSysmoduleLoadModule(u32);
extern int sceSysmoduleUnloadModule(u32);
extern int sceAcmContextCreate(u32*);
extern int sceAcmBatchStartBuffers(u32, u32, const void*, void*, u32*);
extern int sceAcmBatchWait(u32, u32, u32);
extern int sceAcm_Panner(void);
extern int sceMouseInit(void);
extern int sceMouseOpen(int, int, int, const void*);
extern int sceMouseRead(int, void*, int);
extern int sceConvertKeycodeGetVirtualKeycode(u64, u64, u64, u64);
extern int sceNetCtlGetInfo(int, void*);
extern int sceSystemGestureInitializePrimitiveTouchRecognizer(const void*);
extern int sceSystemGestureFinalizePrimitiveTouchRecognizer(void);
extern int sceSystemGestureOpen(int, const void*);
extern int sceSystemGestureCreateTouchRecognizer(int, void*, int, const void*, const void*);
extern int sceSystemGestureAppendTouchRecognizer(int, void*);
extern int sceSystemGestureRemoveTouchRecognizer(int, void*);
extern int sceSystemGestureClose(int);
extern int sceSystemGestureUpdatePrimitiveTouchRecognizer(int, const void*);
extern int sceSystemGestureUpdateAllTouchRecognizer(int);
extern int sceSystemGestureGetTouchEventsCount(int, const void*);
extern int sceSystemGestureGetTouchEventByIndex(int, const void*, u32, void*);
extern int sceNpGameIntentInitialize(const void*);
extern int sceNpGameIntentTerminate(void);
extern int sceNpGameIntentReceiveIntent(void*);
extern int sceNpGameIntentGetPropertyValueString(const void*, const char*, char*, u64);
extern int sceImeKeyboardOpen(int, const void*);
extern int sceImeKeyboardClose(int);
extern int sceImeKeyboardGetInfo(u32, void*);
extern int sceImeUpdate(void*);
extern int sceImeDialogInit(const void*, const void*);
extern int sceImeDialogTerm(void);
extern int sceImeDialogAbort(void);
extern int sceImeDialogGetStatus(void);
extern int sceImeDialogGetResult(void*);
extern int sceImeDialogGetPanelSizeExtended(const void*, const void*, u32*, u32*);
extern int sceMsgDialogInitialize(void);
extern int sceMsgDialogOpen(const void*);
extern int sceMsgDialogTerminate(void);
extern int sceMsgDialogUpdateStatus(void);
extern int sceErrorDialogInitialize(void);
extern int sceErrorDialogOpen(const void*);
extern int sceErrorDialogTerminate(void);
extern int sceErrorDialogUpdateStatus(void);
extern int sceSigninDialogInitialize(void);
extern int sceSigninDialogOpen(const void*);
extern int sceSigninDialogTerminate(void);
extern int sceSigninDialogUpdateStatus(void);
extern int sceNpCommerceDialogInitialize(void);
extern int sceNpCommerceDialogOpen(const void*);
extern int sceNpCommerceDialogUpdateStatus(void);
extern int sceNpCommerceDialogTerminate(void);
extern int sceNpCommerceDialogGetResult(void*);
extern int scePlayerInvitationDialogInitialize(void);
extern int scePlayerInvitationDialogOpen(const void*);
extern int scePlayerInvitationDialogUpdateStatus(void);
extern int scePlayerInvitationDialogTerminate(void);
extern int sceSaveDataDialogInitialize(void);
extern int sceSaveDataDialogOpen(const void*);
extern int sceSaveDataDialogTerminate(void);
extern int sceSaveDataDialogUpdateStatus(void);
extern int sceSaveDataInitialize3(const void*);
extern int sceSaveDataTerminate(void);
extern int sceSaveDataCreateTransactionResource(u32);
extern int sceSaveDataCommit(const void*);
extern int sceSaveDataPrepare(const void*, const void*);
extern int sceAppContentInitialize(const void*, void*);
extern int sceAppContentAddcontMount(u32, const void*, void*);
extern int sceAppContentTemporaryDataMount2(u32, void*);
extern int sceAppContentTemporaryDataUnmount(const void*);
extern int sceAppContentAppParamGetInt(u32, int*);
extern int sceNpUniversalDataSystemInitialize(const void*);
extern int sceNpUniversalDataSystemCreateHandle(int*);
extern int sceNpUniversalDataSystemDestroyHandle(int);
extern int sceNpUniversalDataSystemCreateContext(int*, int, u32, u64);
extern int sceNpUniversalDataSystemDestroyContext(int);
extern int sceNpUniversalDataSystemRegisterContext(int, int, u64);
extern int sceNpUniversalDataSystemCreateEvent(const char*, const void*, void**, void**);
extern int sceNpUniversalDataSystemDestroyEvent(void*);
extern int sceNpUniversalDataSystemPostEvent(int, int, const void*, u64);
extern int sceNpUniversalDataSystemEventPropertyObjectSetString(void*, const char*, const char*);
extern int sceNpUniversalDataSystemEventPropertyObjectSetInt32(void*, const char*, int);
extern int sceNpUniversalDataSystemEventPropertyObjectSetUInt32(void*, const char*, u32);
extern int sceNpUniversalDataSystemEventPropertyObjectSetFloat32(void*, const char*, float);
extern int sceNpUniversalDataSystemEventPropertyObjectSetArray(void*, const char*, const void*, void**);
extern int sceNpUniversalDataSystemEventPropertyArraySetString(void*, const char*);
extern int sceNpTrophy2CreateContext(int*, int, u32, u64);
extern int sceNpTrophy2DestroyContext(int);
extern int sceNpTrophy2RegisterContext(int, int, u64);
extern int sceNpTrophy2CreateHandle(int*);
extern int sceNpTrophy2DestroyHandle(int);
extern int sceNpTrophy2GetTrophyInfo(int, int, int, void*, void*);
extern int sceNpTrophy2GetGameInfo(int, int, void*, void*);
extern int sceRtcFormatRFC3339(char*, const u64*, int);
extern int sceRtcParseRFC3339(u64*, const char*);
extern int sceNpAuthGetIdTokenV3(int, const void*, void*);

#define CK(condition) do { if (!(condition)) return __LINE__; } while (0)
#define ERR(code) ((int)(code##u))

static void fill(volatile void* target, u64 size) {
    for (u64 index = 0; index < size; ++index) ((volatile u8*)target)[index] = 0xa7;
}

static int untouched(const volatile void* target, u64 from, u64 size) {
    for (u64 index = from; index < from + size; ++index)
        if (((const volatile u8*)target)[index] != 0xa7) return 0;
    return 1;
}

static int zeroed(const volatile void* target, u64 from, u64 size) {
    for (u64 index = from; index < from + size; ++index)
        if (((const volatile u8*)target)[index]) return 0;
    return 1;
}

static u32 word(const volatile void* target, u64 offset) {
    const volatile u8* bytes = (const volatile u8*)target + offset;
    return (u32)bytes[0] | (u32)bytes[1] << 8 | (u32)bytes[2] << 16 | (u32)bytes[3] << 24;
}

static int same(const volatile char* left, const char* right) {
    while (*right) if (*left++ != *right++) return 0;
    return *left == 0;
}

static const u8 dummy[256] = {1};
static const u8* volatile param = dummy;

static int systemChecks(void) {
    // libSceSysmodule: load counts per module id; an unloaded module reports 0x805a1001.
    CK(sceSysmoduleLoadModule(1) == 0);
    CK(sceSysmoduleUnloadModule(1) == 0);
    CK(sceSysmoduleUnloadModule(1) == ERR(0x805a1001));
    CK(sceSysmoduleLoadModule(0x7ff0) == 0);

    // libSceAcm: ids come from one counter; batches are no-ops.
    volatile u32 context = 0xa7a7a7a7u, batch = 0xa7a7a7a7u;
    CK(sceAcmContextCreate(0) == ERR(0x80e30002));
    CK(sceAcmContextCreate((u32*)&context) == 0 && context != 0xa7a7a7a7u);
    CK(sceAcmBatchStartBuffers(context, 0, 0, 0, 0) == ERR(0x80e30002));
    CK(sceAcmBatchStartBuffers(context, 0, param, 0, (u32*)&batch) == 0 && batch != 0xa7a7a7a7u && batch != context);
    CK(sceAcmBatchWait(context, batch, 0) == 0);
    CK(sceAcm_Panner() == 0);

    // libSceConvertKeycode and libSceNetCtl: upstream reports invalid address / not connected.
    CK(sceConvertKeycodeGetVirtualKeycode(1, 2, 3, 4) == ERR(0x80bc0031));
    CK(sceNetCtlGetInfo(0, 0) == ERR(0x80412108));

    // libSceRtc: parse, then format in two zones. Only the string and its NUL are written.
    volatile u64 tick = 0x5a5a5a5a5a5a5a5aul;
    CK(sceRtcParseRFC3339(0, "2000-03-04T05:06:07.08+09:30") == ERR(0x80b50002));
    CK(sceRtcParseRFC3339((u64*)&tick, "2000-03-04T05:06:07.08+09:30") == 0 && tick != 0x5a5a5a5a5a5a5a5aul);
    volatile char text[64];
    fill(text, sizeof text);
    CK(sceRtcFormatRFC3339((char*)text, (const u64*)&tick, 570) == 0);
    CK(same(text, "2000-03-04T05:06:07.08+09:30") && untouched(text, 29, 35));
    fill(text, sizeof text);
    CK(sceRtcFormatRFC3339((char*)text, (const u64*)&tick, 0) == 0);
    CK(same(text, "2000-03-03T19:36:07.08Z") && untouched(text, 24, 40));
    fill(text, sizeof text);
    CK(sceRtcFormatRFC3339((char*)text, (const u64*)&tick, 1440) == ERR(0x80b50003) && untouched(text, 0, sizeof text));
    return 0;
}

static int inputChecks(void) {
    // libSceMouse: handle 1, one initial sample, bounded reads.
    volatile u8 samples[40 * 4 + 8];
    fill(samples, sizeof samples);
    CK(sceMouseOpen(1, 0, 0, 0) == ERR(0x80df0005));
    CK(sceMouseInit() == 0);
    CK(sceMouseOpen(-1, 0, 0, 0) == ERR(0x80df0001));
    CK(sceMouseOpen(1, 0, 0, 0) == 1);
    CK(sceMouseOpen(1, 0, 0, 0) == ERR(0x80df0004));
    CK(sceMouseRead(1, (void*)samples, 0) == ERR(0x80df0001));
    CK(sceMouseRead(2, (void*)samples, 4) == ERR(0x80df0003));
    CK(sceMouseRead(1, (void*)samples, 4) == 1);
    CK(untouched(samples, 40, sizeof samples - 40));
    CK(sceMouseRead(1, (void*)samples, 4) == 0);

    // libSceSystemGesture: one gesture handle, recognizers that never report events.
    CK(sceSystemGestureInitializePrimitiveTouchRecognizer(param) == 0);
    CK(sceSystemGestureOpen(0, param) == 1);
    CK(sceSystemGestureClose(2) == ERR(0x80d10003));
    CK(sceSystemGestureCreateTouchRecognizer(2, (void*)param, 0, param, 0) == ERR(0x80d10003));
    CK(sceSystemGestureCreateTouchRecognizer(1, 0, 0, param, 0) == ERR(0x80d10002));
    CK(sceSystemGestureCreateTouchRecognizer(1, (void*)param, 0, param, 0) == 0);
    CK(sceSystemGestureAppendTouchRecognizer(1, (void*)param) == 0);
    CK(sceSystemGestureUpdatePrimitiveTouchRecognizer(1, param) == 0);
    CK(sceSystemGestureUpdatePrimitiveTouchRecognizer(2, param) == ERR(0x80d10003));
    CK(sceSystemGestureUpdateAllTouchRecognizer(1) == 0);
    CK(sceSystemGestureGetTouchEventsCount(1, param) == 0);
    CK(sceSystemGestureGetTouchEventsCount(2, param) == ERR(0x80d10003));
    CK(sceSystemGestureGetTouchEventByIndex(1, param, 0, (void*)param) == ERR(0x80d10005));
    CK(sceSystemGestureRemoveTouchRecognizer(1, (void*)param) == 0);
    CK(sceSystemGestureClose(1) == 0);
    CK(sceSystemGestureFinalizePrimitiveTouchRecognizer() == 0);

    // libSceIme: keyboards are tracked per user and never produce events.
    CK(sceImeKeyboardGetInfo(0, (void*)param) == ERR(0x80bc0002));
    CK(sceImeKeyboardOpen(5, param) == 0);
    CK(sceImeKeyboardGetInfo(0, (void*)param) == ERR(0x80bc0023));
    CK(sceImeKeyboardGetInfo(0, 0) == ERR(0x80bc0031));
    CK(sceImeUpdate((void*)0x1000) == 0);
    CK(sceImeKeyboardClose(5) == 0);
    CK(sceImeKeyboardGetInfo(0, (void*)param) == ERR(0x80bc0002));

    // libSceImeDialog: finishes on the second status poll, text unchanged.
    volatile u8 result[24];
    volatile u32 size[2] = {0xa7a7a7a7u, 0xa7a7a7a7u};
    CK(sceImeDialogGetStatus() == 0);
    CK(sceImeDialogTerm() == ERR(0x80bc0002));
    CK(sceImeDialogInit(0, 0) == 0);
    CK(sceImeDialogGetStatus() == 1 && sceImeDialogGetStatus() == 2);
    fill(result, sizeof result);
    CK(sceImeDialogGetResult((void*)result) == 0 && word(result, 0) == 0 && zeroed(result, 4, 12) && untouched(result, 16, 8));
    CK(sceImeDialogGetResult(0) == ERR(0x80bc0002));
    CK(sceImeDialogGetPanelSizeExtended(0, 0, (u32*)&size[0], (u32*)&size[1]) == 0 && size[0] == 1920 && size[1] == 1080);
    CK(sceImeDialogTerm() == 0);
    CK(sceImeDialogInit(0, 0) == 0 && sceImeDialogAbort() == 0 && sceImeDialogAbort() == ERR(0x80bc0002));
    fill(result, sizeof result);
    CK(sceImeDialogGetResult((void*)result) == 0 && word(result, 0) == 2);
    CK(sceImeDialogTerm() == 0);
    return 0;
}

static int dialogChecks(void) {
    // libSceMsgDialog: opening finishes at once.
    CK(sceMsgDialogInitialize() == 0);
    CK(sceMsgDialogOpen(0) == ERR(0x80b8000d));
    CK(sceMsgDialogOpen(param) == 0);
    CK(sceMsgDialogUpdateStatus() == 3);
    CK(sceMsgDialogTerminate() == 0);

    // libSceErrorDialog: the 16-byte parameter block is validated, then runs until polled.
    volatile u32 block[4] = {16, 0x80010001u, 3, 0};
    CK(sceErrorDialogOpen((const void*)block) == ERR(0x80ed0005));
    CK(sceErrorDialogInitialize() == 0 && sceErrorDialogInitialize() == ERR(0x80ed0002));
    CK(sceErrorDialogOpen(0) == ERR(0x80ed0003));
    block[0] = 12;
    CK(sceErrorDialogOpen((const void*)block) == ERR(0x80ed0003));
    block[0] = 16;
    CK(sceErrorDialogOpen((const void*)block) == 0);
    CK(sceErrorDialogUpdateStatus() == 3);
    CK(sceErrorDialogTerminate() == 0 && sceErrorDialogTerminate() == ERR(0x80ed0001));

    // libSceSigninDialog.
    CK(sceSigninDialogOpen(param) == ERR(0x80b80003));
    CK(sceSigninDialogInitialize() == 0 && sceSigninDialogInitialize() == ERR(0x80b80004));
    CK(sceSigninDialogOpen(0) == ERR(0x80b8000d));
    CK(sceSigninDialogOpen(param) == 0 && sceSigninDialogUpdateStatus() == 3);
    CK(sceSigninDialogTerminate() == 0 && sceSigninDialogTerminate() == ERR(0x80b80003));

    // libSceNpCommerce: the result is cancelled with the authorized byte clear; one byte past it is kept.
    volatile u8 commerce[8];
    fill(commerce, sizeof commerce);
    CK(sceNpCommerceDialogOpen(param) == ERR(0x80b80003));
    CK(sceNpCommerceDialogInitialize() == 0 && sceNpCommerceDialogInitialize() == ERR(0x80b80004));
    CK(sceNpCommerceDialogGetResult((void*)commerce) == ERR(0x80b80006) && untouched(commerce, 0, sizeof commerce));
    CK(sceNpCommerceDialogOpen(0) == ERR(0x80b80009));
    CK(sceNpCommerceDialogOpen(param) == 0 && sceNpCommerceDialogUpdateStatus() == 3);
    CK(sceNpCommerceDialogGetResult(0) == ERR(0x80b80009));
    CK(sceNpCommerceDialogGetResult((void*)commerce) == 0);
    CK(word(commerce, 0) == 1 && commerce[4] == 0 && untouched(commerce, 5, 3));
    CK(sceNpCommerceDialogTerminate() == 0);

    // libScePlayerInvitationDialog: Open runs, the first status poll finishes.
    CK(scePlayerInvitationDialogInitialize() == 0);
    CK(scePlayerInvitationDialogOpen(param) == 0);
    CK(scePlayerInvitationDialogUpdateStatus() == 3);
    CK(scePlayerInvitationDialogTerminate() == 0);

    // libSceSaveDataDialog: progress bars keep running, everything else finishes at once.
    volatile u8 saveParam[96];
    fill(saveParam, sizeof saveParam);
    ((volatile u32*)saveParam)[13] = 5;  // mode at byte 52: progress bar
    CK(sceSaveDataDialogOpen((const void*)saveParam) == ERR(0x80b80006));
    CK(sceSaveDataDialogInitialize() == 0 && sceSaveDataDialogInitialize() == ERR(0x80b80004));
    CK(sceSaveDataDialogOpen(0) == ERR(0x80b8000d));
    CK(sceSaveDataDialogOpen((const void*)saveParam) == 0 && sceSaveDataDialogUpdateStatus() == 2);
    CK(sceSaveDataDialogOpen((const void*)saveParam) == ERR(0x80b80006));
    CK(sceSaveDataDialogTerminate() == 0 && sceSaveDataDialogUpdateStatus() == 0);
    CK(sceSaveDataDialogInitialize() == 0);
    ((volatile u32*)saveParam)[13] = 1;
    CK(sceSaveDataDialogOpen((const void*)saveParam) == 0 && sceSaveDataDialogUpdateStatus() == 3);
    CK(sceSaveDataDialogTerminate() == 0);
    return 0;
}

static int storageChecks(void) {
    // libSceSaveData: initialisation is counted; transaction resources count up from 1.
    CK(sceSaveDataTerminate() == -2137063423);
    CK(sceSaveDataInitialize3(0) == 0);
    CK(sceSaveDataCreateTransactionResource(4096) == 1 && sceSaveDataCreateTransactionResource(4096) == 2);
    CK(sceSaveDataPrepare(0, 0) == 0 && sceSaveDataCommit(0) == 0);
    CK(sceSaveDataTerminate() == 0 && sceSaveDataTerminate() == -2137063423);

    // libSceAppContent: add-ons are not entitled; temporary data mounts once at /temp0.
    volatile u8 boot[48], mount[24];
    fill(boot, sizeof boot);
    CK(sceAppContentInitialize(0, 0) == ERR(0x80d90002));
    CK(sceAppContentInitialize(0, (void*)boot) == 0 && zeroed(boot, 0, 40) && untouched(boot, 40, 8));
    volatile int value = 0x5a5a5a5a;
    CK(sceAppContentAppParamGetInt(0, (int*)&value) == 0 && value == 3);
    CK(sceAppContentAppParamGetInt(2, (int*)&value) == 0 && value == 0);
    CK(sceAppContentAppParamGetInt(5, (int*)&value) == ERR(0x80d90002) && value == 0);
    CK(sceAppContentAppParamGetInt(0, 0) == ERR(0x80d90002));
    fill(mount, sizeof mount);
    CK(sceAppContentAddcontMount(0, 0, (void*)mount) == ERR(0x80d90002));
    CK(sceAppContentAddcontMount(0, param, (void*)mount) == ERR(0x80d90005) && untouched(mount, 0, sizeof mount));
    CK(sceAppContentTemporaryDataUnmount((const void*)mount) == ERR(0x80d90004));
    CK(sceAppContentTemporaryDataMount2(2, (void*)mount) == ERR(0x80d90002));
    CK(sceAppContentTemporaryDataMount2(0, (void*)mount) == 0);
    CK(same((const volatile char*)mount, "/temp0") && zeroed(mount, 6, 10) && untouched(mount, 16, 8));
    CK(sceAppContentTemporaryDataMount2(0, (void*)mount) == ERR(0x80d90003));
    CK(sceAppContentTemporaryDataUnmount((const void*)mount) == 0);
    CK(sceAppContentTemporaryDataUnmount((const void*)mount) == ERR(0x80d90004));
    return 0;
}

static int networkChecks(void) {
    // libSceNpAuth: the id token call reports the signed-out user like the other auth exports.
    CK(sceNpAuthGetIdTokenV3(1, 0, (void*)param) == ERR(0x80550003));
    CK(sceNpAuthGetIdTokenV3(1, param, (void*)param) == ERR(0x80550006));

    // libSceNpGameIntent: no intent is ever pending.
    volatile u8 info[1400];
    fill(info, sizeof info);
    CK(sceNpGameIntentInitialize(0) == 0);
    CK(sceNpGameIntentReceiveIntent((void*)info) == -2141898746);
    CK(word(info, 8) == 0xffffffffu && zeroed(info, 12, 64) && untouched(info, 76, 7) && untouched(info, 1376, 24));
    volatile char value[16];
    fill(value, sizeof value);
    CK(sceNpGameIntentGetPropertyValueString(param, "key", (char*)value, sizeof value) == -2141898745);
    CK(value[0] == 0 && untouched(value, 1, 15));
    CK(sceNpGameIntentTerminate() == 0);

    // libSceNpUniversalDataSystem: handles, contexts and events succeed; property objects are tokens.
    volatile int handle = 0x5a5a5a5a, id = 0x5a5a5a5a;
    CK(sceNpUniversalDataSystemInitialize(param) == 0);
    CK(sceNpUniversalDataSystemCreateHandle((int*)&handle) == 0 && handle == 1);
    CK(sceNpUniversalDataSystemCreateContext((int*)&id, 1, 0, 0) == 0 && id == 1);
    CK(sceNpUniversalDataSystemRegisterContext(id, handle, 0) == 0);
    void* volatile event = 0;
    void* volatile properties = 0;
    CK(sceNpUniversalDataSystemCreateEvent("event", 0, (void**)&event, (void**)&properties) == 0);
    CK(event != 0 && properties != 0);
    CK(sceNpUniversalDataSystemEventPropertyObjectSetString(properties, "key", "value") == 0);
    CK(sceNpUniversalDataSystemEventPropertyObjectSetInt32(properties, "key", -5) == 0);
    CK(sceNpUniversalDataSystemEventPropertyObjectSetUInt32(properties, "key", 5) == 0);
    CK(sceNpUniversalDataSystemEventPropertyObjectSetFloat32(properties, "key", 1.5f) == 0);
    void* volatile array = 0;
    CK(sceNpUniversalDataSystemEventPropertyObjectSetArray(properties, "key", 0, (void**)&array) == 0 && array != 0);
    CK(sceNpUniversalDataSystemEventPropertyArraySetString(array, "value") == 0);
    CK(sceNpUniversalDataSystemPostEvent(id, handle, event, 0) == 0);
    CK(sceNpUniversalDataSystemDestroyEvent(event) == 0);
    CK(sceNpUniversalDataSystemDestroyContext(id) == 0 && sceNpUniversalDataSystemDestroyHandle(handle) == 0);

    // libSceNpTrophy2: one group, one bronze trophy, nothing unlocked.
    CK(sceNpTrophy2CreateHandle((int*)&handle) == 0 && handle == 1);
    CK(sceNpTrophy2CreateContext((int*)&id, 1, 0, 0) == 0 && id == 1);
    CK(sceNpTrophy2RegisterContext(id, handle, 0) == 0);
    volatile u8 game[152 + 8], gameData[24 + 8], trophy[1300 + 8], trophyData[24 + 8];
    fill(game, sizeof game);
    fill(gameData, sizeof gameData);
    CK(sceNpTrophy2GetGameInfo(id, handle, (void*)game, (void*)gameData) == 0);
    CK(word(game, 0) == 1 && word(game, 4) == 1 && word(game, 8) == 0 && word(game, 20) == 1);
    CK(same((const volatile char*)game + 24, "Game") && untouched(game, 152, 8));
    CK(word(gameData, 0) == 0 && word(gameData, 20) == 0 && untouched(gameData, 24, 8));
    fill(trophy, sizeof trophy);
    fill(trophyData, sizeof trophyData);
    CK(sceNpTrophy2GetTrophyInfo(id, handle, 7, (void*)trophy, (void*)trophyData) == 0);
    CK(word(trophy, 0) == 7 && word(trophy, 4) == 4 && same((const volatile char*)trophy + 20, "Trophy") && untouched(trophy, 1300, 8));
    CK(word(trophyData, 0) == 7 && trophyData[4] == 0 && untouched(trophyData, 24, 8));
    CK(sceNpTrophy2DestroyContext(id) == 0 && sceNpTrophy2DestroyHandle(handle) == 0);
    return 0;
}

EXPORT int SceGuestMain(u64 argc, char** argv) {
    (void)argv;
    if (argc != 1) return 81;
    if (LeafSystemLibsGuestValue() != 11) return 82;
    int status = systemChecks();
    if (!status) status = inputChecks();
    if (!status) status = dialogChecks();
    if (!status) status = storageChecks();
    if (!status) status = networkChecks();
    return status;
}

#endif
