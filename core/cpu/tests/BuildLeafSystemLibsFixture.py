"""Package the leaf system libraries fixture with the retained platform packager.

Like BuildUpstreamNpBridgeFixture.py, this keeps the packager's linker validation and
parameterizes only names and scopes. Every import is a system, dialog, input or NP leaf
function the production resolver chain must serve, so a strict-imports run proves each
one resolves and every module the title lists in DT_NEEDED is provided.
"""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
helper = Path(__file__).resolve().parent / "BuildPlatformServiceFixture.py"
sys.path.insert(0, str(helper.parent))
from BuildSceModulesFixture import nid  # noqa: E402

GUEST = "LeafSystemLibsGuest"
ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"

# import module -> (import library, module file, module version, exports). Module versions and
# library names are the ones the target title was linked with.
LIBRARIES = {
    "libSceAppContentUtil": ("libSceAppContent", "libSceAppContent.prx", 0x101, (
        "sceAppContentInitialize", "sceAppContentAddcontMount", "sceAppContentTemporaryDataMount2",
        "sceAppContentTemporaryDataUnmount", "sceAppContentAppParamGetInt")),
    "libSceAcm": ("libSceAcm", "libSceAcm.prx", 0x101, (
        "sceAcmContextCreate", "sceAcmBatchStartBuffers", "sceAcmBatchWait", "sceAcm_Panner")),
    "libSceConvertKeycode": ("libSceConvertKeycode", "libSceConvertKeycode.prx", 0x100, (
        "sceConvertKeycodeGetVirtualKeycode",)),
    "libSceErrorDialog": ("libSceErrorDialog", "libSceErrorDialog.prx", 0x101, (
        "sceErrorDialogInitialize", "sceErrorDialogOpen", "sceErrorDialogTerminate", "sceErrorDialogUpdateStatus")),
    "libSceIme": ("libSceIme", "libSceIme.prx", 0x101, (
        "sceImeKeyboardOpen", "sceImeKeyboardClose", "sceImeKeyboardGetInfo", "sceImeUpdate")),
    "libSceImeDialog": ("libSceImeDialog", "libSceImeDialog.prx", 0x101, (
        "sceImeDialogInit", "sceImeDialogTerm", "sceImeDialogAbort", "sceImeDialogGetStatus",
        "sceImeDialogGetResult", "sceImeDialogGetPanelSizeExtended")),
    "libSceMouse": ("libSceMouse", "libSceMouse.prx", 0x101, ("sceMouseInit", "sceMouseOpen", "sceMouseRead")),
    "libSceMsgDialog": ("libSceMsgDialog.native", "libSceMsgDialog.native.prx", 0x101, (
        "sceMsgDialogInitialize", "sceMsgDialogOpen", "sceMsgDialogTerminate", "sceMsgDialogUpdateStatus")),
    "libSceNetCtl": ("libSceNetCtl", "libSceNetCtl.prx", 0x101, ("sceNetCtlGetInfo",)),
    "libSceNpAuth": ("libSceNpAuth", "libSceNpAuth.prx", 0x101, ("sceNpAuthGetIdTokenV3",)),
    "libSceNpCommerce": ("libSceNpCommerce", "libSceNpCommerce.prx", 0x101, (
        "sceNpCommerceDialogInitialize", "sceNpCommerceDialogOpen", "sceNpCommerceDialogUpdateStatus",
        "sceNpCommerceDialogTerminate", "sceNpCommerceDialogGetResult")),
    "libSceNpGameIntent": ("libSceNpGameIntent", "libSceNpGameIntent.prx", 0x101, (
        "sceNpGameIntentInitialize", "sceNpGameIntentTerminate", "sceNpGameIntentReceiveIntent",
        "sceNpGameIntentGetPropertyValueString")),
    "libSceNpTrophy2": ("libSceNpTrophy2", "libSceNpTrophy2.prx", 0x101, (
        "sceNpTrophy2CreateContext", "sceNpTrophy2DestroyContext", "sceNpTrophy2RegisterContext",
        "sceNpTrophy2CreateHandle", "sceNpTrophy2DestroyHandle", "sceNpTrophy2GetTrophyInfo",
        "sceNpTrophy2GetGameInfo")),
    "libSceNpUniversalDataSystem": ("libSceNpUniversalDataSystem", "libSceNpUniversalDataSystem.prx", 0x101, (
        "sceNpUniversalDataSystemInitialize", "sceNpUniversalDataSystemCreateHandle",
        "sceNpUniversalDataSystemDestroyHandle", "sceNpUniversalDataSystemCreateContext",
        "sceNpUniversalDataSystemDestroyContext", "sceNpUniversalDataSystemRegisterContext",
        "sceNpUniversalDataSystemCreateEvent", "sceNpUniversalDataSystemDestroyEvent",
        "sceNpUniversalDataSystemPostEvent", "sceNpUniversalDataSystemEventPropertyObjectSetString",
        "sceNpUniversalDataSystemEventPropertyObjectSetInt32", "sceNpUniversalDataSystemEventPropertyObjectSetUInt32",
        "sceNpUniversalDataSystemEventPropertyObjectSetFloat32", "sceNpUniversalDataSystemEventPropertyObjectSetArray",
        "sceNpUniversalDataSystemEventPropertyArraySetString")),
    "libScePlayerInvitationDialog": ("libScePlayerInvitationDialog", "libScePlayerInvitationDialog.prx", 0x101, (
        "scePlayerInvitationDialogInitialize", "scePlayerInvitationDialogOpen",
        "scePlayerInvitationDialogUpdateStatus", "scePlayerInvitationDialogTerminate")),
    "libSceRtc": ("libSceRtc", "libSceRtc.prx", 0x101, ("sceRtcFormatRFC3339", "sceRtcParseRFC3339")),
    "libSceSaveDataDialog": ("libSceSaveDataDialog.native", "libSceSaveDataDialog.native.prx", 0x101, (
        "sceSaveDataDialogInitialize", "sceSaveDataDialogOpen", "sceSaveDataDialogTerminate",
        "sceSaveDataDialogUpdateStatus")),
    "libSceSaveData_native": ("libSceSaveData_native", "libSceSaveData.native.prx", 0x101, (
        "sceSaveDataInitialize3", "sceSaveDataTerminate", "sceSaveDataCreateTransactionResource",
        "sceSaveDataCommit", "sceSaveDataPrepare")),
    "libSceSigninDialog": ("libSceSigninDialog", "libSceSigninDialog.prx", 0x101, (
        "sceSigninDialogInitialize", "sceSigninDialogOpen", "sceSigninDialogTerminate", "sceSigninDialogUpdateStatus")),
    "libSceSysmodule": ("libSceSysmodule", "libSceSysmodule.prx", 0x101, (
        "sceSysmoduleLoadModule", "sceSysmoduleUnloadModule")),
    "libSceSystemGesture": ("libSceSystemGesture", "libSceSystemGesture.prx", 0x101, (
        "sceSystemGestureInitializePrimitiveTouchRecognizer", "sceSystemGestureFinalizePrimitiveTouchRecognizer",
        "sceSystemGestureOpen", "sceSystemGestureCreateTouchRecognizer", "sceSystemGestureAppendTouchRecognizer",
        "sceSystemGestureRemoveTouchRecognizer", "sceSystemGestureClose",
        "sceSystemGestureUpdatePrimitiveTouchRecognizer", "sceSystemGestureUpdateAllTouchRecognizer",
        "sceSystemGestureGetTouchEventsCount", "sceSystemGestureGetTouchEventByIndex")),
}
SCOPES = {1: GUEST, 2: "libc"}
IMPORTS = {"LeafSystemLibsGuestValue": 1}
for scope, (module, (library, _, version, names)) in enumerate(LIBRARIES.items(), start=3):
    SCOPES[scope] = (module, library, version)
    IMPORTS.update({name: scope for name in names})
HOST_FILES = tuple(file for _, file, _, _ in LIBRARIES.values())


def packager():
    source = helper.read_text()
    replacements = {
        'expected_exports = {"_start", "SceGuestMain"} if main else {"PlatformServiceGuestMath"}':
            'expected_exports = {"_start", "SceGuestMain"} if main else {"LeafSystemLibsGuestValue"}',
        'for filename in (GUEST_FILE, "libSceNpManager.prx", "libSceNet.prx", "libSceCommonDialog.prx"):':
            'for filename in (GUEST_FILE, *HOST_FILES):',
        'qualifier = "ABCDEF"[scope]': 'qualifier = ALPHABET[scope]',
        """            name_offset = new_string(name)
            sce_tags += [(0x61000045, (scope << 48) | (0x101 << 32) | name_offset),
                         (0x61000049, (scope << 48) | (1 << 32) | name_offset),
                         (0x61000019, scope << 48)]""":
        """            module_name, library_name, version = name if isinstance(name, tuple) else (name, name, 0x101)
            name_offset = new_string(module_name)
            library_offset = new_string(library_name)
            sce_tags += [(0x61000045, (scope << 48) | (version << 32) | name_offset),
                         (0x61000049, (scope << 48) | (1 << 32) | library_offset),
                         (0x61000019, scope << 48)]""",
    }
    for old, new in replacements.items():
        if source.count(old) != 1:
            raise RuntimeError("Retained packager anchor changed: " + old)
        source = source.replace(old, new)
    spec = importlib.util.spec_from_file_location("leaf_system_libs_packager", helper)
    module = importlib.util.module_from_spec(spec)
    exec(compile(source, str(helper), "exec"), module.__dict__)
    module.GUEST_NAME = GUEST
    module.GUEST_FILE = GUEST + ".prx"
    module.MAIN_NAME = "LeafSystemLibsHomebrew"
    module.SCOPES = SCOPES
    module.ALPHABET = ALPHABET
    module.IMPORTS = {"uMei1W9uyNo": ("uMei1W9uyNo", 2), **{name: (nid(name), scope) for name, scope in IMPORTS.items()}}
    module.HOST_FILES = HOST_FILES
    return module


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("Usage: BuildLeafSystemLibsFixture.py guest-linked.elf main-linked.elf guest.prx main.elf")
    module = packager()
    module.package(sys.argv[1], sys.argv[3], False)
    module.package(sys.argv[2], sys.argv[4], True)
