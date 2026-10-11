"""Package the upstream NP bridge fixture with the retained platform packager.

Like BuildLazyImportFixture.py, this keeps the packager's linker validation and
parameterizes only names and scopes. Every import is a libSceNp* function served by
the upstream prx bridge, so a strict-imports run proves each one resolves.
"""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
helper = Path(__file__).resolve().parent / "BuildPlatformServiceFixture.py"
sys.path.insert(0, str(helper.parent))
from BuildSceModulesFixture import nid  # noqa: E402

GUEST = "UpstreamNpBridgeGuest"
SCOPES = {1: GUEST, 2: "libc", 3: "libSceNpManager", 4: "libSceNpWebApi2",
          5: "libSceNpAuth", 6: "libSceNpEntitlementAccess"}
IMPORTS = {
    "UpstreamNpBridgeGuestValue": 1,
    **{name: 3 for name in ("sceNpCreateRequest", "sceNpDeleteRequest", "sceNpSetTimeout",
                            "sceNpCheckNpAvailability", "sceNpGetState", "sceNpHasSignedUp",
                            "sceNpPollAsync", "sceNpGetAccountIdA")},
    **{name: 4 for name in ("sceNpWebApi2Initialize", "sceNpWebApi2CreateUserContext",
                            "sceNpWebApi2CreateRequest", "sceNpWebApi2SendRequest",
                            "sceNpWebApi2CheckTimeout")},
    **{name: 5 for name in ("sceNpAuthCreateAsyncRequest", "sceNpAuthPollAsync",
                            "sceNpAuthGetAuthorizationCodeV3")},
    **{name: 6 for name in ("sceNpEntitlementAccessInitialize", "sceNpEntitlementAccessGetSkuFlag",
                            "sceNpEntitlementAccessGenerateTransactionId",
                            "sceNpEntitlementAccessGetAddcontEntitlementInfo",
                            "sceNpEntitlementAccessGetAddcontEntitlementInfoList")},
}
HOST_FILES = ("libSceNpManager.prx", "libSceNpWebApi2.prx", "libSceNpAuth.prx", "libSceNpEntitlementAccess.prx")


def packager():
    source = helper.read_text()
    replacements = {
        'expected_exports = {"_start", "SceGuestMain"} if main else {"PlatformServiceGuestMath"}':
            'expected_exports = {"_start", "SceGuestMain"} if main else {"UpstreamNpBridgeGuestValue"}',
        'for filename in (GUEST_FILE, "libSceNpManager.prx", "libSceNet.prx", "libSceCommonDialog.prx"):':
            'for filename in (GUEST_FILE, *HOST_FILES):',
        'qualifier = "ABCDEF"[scope]': 'qualifier = "ABCDEFG"[scope]',
    }
    for old, new in replacements.items():
        if source.count(old) != 1:
            raise RuntimeError("Retained packager anchor changed: " + old)
        source = source.replace(old, new)
    spec = importlib.util.spec_from_file_location("upstream_np_bridge_packager", helper)
    module = importlib.util.module_from_spec(spec)
    exec(compile(source, str(helper), "exec"), module.__dict__)
    module.GUEST_NAME = GUEST
    module.GUEST_FILE = GUEST + ".prx"
    module.MAIN_NAME = "UpstreamNpBridgeHomebrew"
    module.SCOPES = SCOPES
    module.IMPORTS = {"uMei1W9uyNo": ("uMei1W9uyNo", 2), **{name: (nid(name), scope) for name, scope in IMPORTS.items()}}
    module.HOST_FILES = HOST_FILES
    return module


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("Usage: BuildUpstreamNpBridgeFixture.py guest-linked.elf main-linked.elf guest.prx main.elf")
    module = packager()
    module.package(sys.argv[1], sys.argv[3], False)
    module.package(sys.argv[2], sys.argv[4], True)
