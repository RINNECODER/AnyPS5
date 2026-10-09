"""Package the lazy-import fixture with the retained platform packager.

Like platform/native-service-integration/BuildNativeServiceFixture.py, this keeps the
packager's genuine linker validation and parameterizes only names, scopes and the
relocation types the fixture needs.
"""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
helper = Path(__file__).resolve().parent / "BuildPlatformServiceFixture.py"
sys.path.insert(0, str(helper.parent))
from BuildSceModulesFixture import nid  # noqa: E402

ABSENT_MODULE = "libSceAnyPS5Absent"
SCOPES = {1: "LazyImportGuest", 2: "libc", 3: "libkernel", 4: ABSENT_MODULE,
          5: "libSceNpWebApi2", 6: "libSceNpManager", 7: "libSceNpWebApi"}
IMPORTS = {"LazyImportGuestValue": 1, "anyps5LazyNeverCalled": 3,
           "anyps5LazyAbsentCall": 4, "anyps5LazyAbsentWeak": 4,
           "sceNpWebApi2Initialize": 5, "sceNpWebApi2CreateUserContext": 5,
           "sceNpWebApi2CreateRequest": 5, "sceNpWebApi2SendRequest": 5,
           "sceNpWebApi2DeleteRequest": 5, "sceNpWebApi2DeleteUserContext": 5,
           "sceNpWebApi2Terminate": 5, "sceNpGetAccountIdA": 6,
           "sceNpGetNpReachabilityState": 6, "sceNpWebApiInitialize": 7, "sceNpWebApiTerminate": 7}


def packager():
    source = helper.read_text()
    replacements = {
        'expected_exports = {"_start", "SceGuestMain"} if main else {"PlatformServiceGuestMath"}':
            'expected_exports = {"_start", "SceGuestMain"} if main else {"LazyImportGuestValue"}',
        'for filename in (GUEST_FILE, "libSceNpManager.prx", "libSceNet.prx", "libSceCommonDialog.prx"):':
            'for filename in (GUEST_FILE, *HOST_FILES):',
        # R_X86_64_64 stores the never-called import's address; GLOB_DAT loads the weak import.
        'types - {7, 8}:': 'types - {1, 6, 7, 8}:',
        'qualifier = "ABCDEF"[scope]': 'qualifier = "ABCDEFGH"[scope]',
    }
    for old, new in replacements.items():
        if source.count(old) != 1:
            raise RuntimeError("Retained packager anchor changed: " + old)
        source = source.replace(old, new)
    spec = importlib.util.spec_from_file_location("lazy_import_packager", helper)
    module = importlib.util.module_from_spec(spec)
    exec(compile(source, str(helper), "exec"), module.__dict__)
    module.GUEST_NAME = "LazyImportGuest"
    module.GUEST_FILE = "LazyImportGuest.prx"
    module.MAIN_NAME = "LazyImportHomebrew"
    module.SCOPES = SCOPES
    module.IMPORTS = {"uMei1W9uyNo": ("uMei1W9uyNo", 2), **{name: (nid(name), scope) for name, scope in IMPORTS.items()}}
    module.HOST_FILES = (ABSENT_MODULE + ".prx", "libSceNpWebApi2.prx", "libSceNpManager.prx", "libkernel.prx")
    return module


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("Usage: BuildLazyImportFixture.py guest-linked.elf main-linked.elf guest.prx main.elf")
    module = packager()
    module.package(sys.argv[1], sys.argv[3], False)
    module.package(sys.argv[2], sys.argv[4], True)
