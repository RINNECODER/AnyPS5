"""Package the libkernel time fixture with the retained platform packager.

Like BuildLazyImportFixture.py, this keeps the packager's genuine linker validation and
parameterizes only names and scopes. The POSIX aliases are imported from the
libScePosix library of the libkernel module, as titles import them, so that scope is a
library-only entry whose imports name the libkernel module id.
"""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
helper = Path(__file__).resolve().parent / "BuildPlatformServiceFixture.py"
sys.path.insert(0, str(helper.parent))
from BuildSceModulesFixture import nid  # noqa: E402

SCOPES = {1: "KernelTimeGuest", 2: "libc", 3: "libkernel", 4: "libScePosix"}
LIBRARY_ONLY = {4: 3}
KERNEL = ("sceKernelUsleep", "sceKernelNanosleep", "sceKernelSleep", "sceKernelGetProcessTime",
          "sceKernelGetProcessTimeCounter", "sceKernelGetProcessTimeCounterFrequency", "sceKernelReadTsc",
          "sceKernelGetTscFrequency", "sceKernelGettimeofday", "sceKernelClockGettime", "sceKernelClockGetres",
          "sceKernelConvertUtcToLocaltime", "sceKernelConvertLocaltimeToUtc")
POSIX = ("clock_gettime", "clock_getres", "gettimeofday", "nanosleep", "usleep", "sleep")
# scePthreadCreate, scePthreadJoin and __error, declared by NID in the fixture source.
THREADS = ("6UgtwV+0zb4", "onNY9Byn-W8", "9BcDykPmo1I")


def packager():
    source = helper.read_text()
    replacements = {
        'expected_exports = {"_start", "SceGuestMain"} if main else {"PlatformServiceGuestMath"}':
            'expected_exports = {"_start", "SceGuestMain"} if main else {"KernelTimeGuestValue"}',
        'for filename in (GUEST_FILE, "libSceNpManager.prx", "libSceNet.prx", "libSceCommonDialog.prx"):':
            'for filename in (GUEST_FILE, *HOST_FILES):',
        '            sce_tags += [(0x61000045, (scope << 48) | (0x101 << 32) | name_offset),\n'
        '                         (0x61000049, (scope << 48) | (1 << 32) | name_offset),\n'
        '                         (0x61000019, scope << 48)]':
            '            sce_tags += [(0x61000049, (scope << 48) | (1 << 32) | name_offset)]\n'
            '            if scope not in LIBRARY_ONLY:\n'
            '                sce_tags += [(0x61000045, (scope << 48) | (0x101 << 32) | name_offset),\n'
            '                             (0x61000019, scope << 48)]',
        'qualifier = "ABCDEF"[scope]\n            qualified = import_nid + "#" + qualifier + "#" + qualifier':
            'qualified = import_nid + "#" + "ABCDEF"[scope] + "#" + "ABCDEF"[LIBRARY_ONLY.get(scope, scope)]',
    }
    for old, new in replacements.items():
        if source.count(old) != 1:
            raise RuntimeError("Retained packager anchor changed: " + old)
        source = source.replace(old, new)
    spec = importlib.util.spec_from_file_location("kernel_time_packager", helper)
    module = importlib.util.module_from_spec(spec)
    exec(compile(source, str(helper), "exec"), module.__dict__)
    module.GUEST_NAME = "KernelTimeGuest"
    module.GUEST_FILE = "KernelTimeGuest.prx"
    module.MAIN_NAME = "KernelTimeHomebrew"
    module.SCOPES = SCOPES
    module.LIBRARY_ONLY = LIBRARY_ONLY
    module.IMPORTS = {"uMei1W9uyNo": ("uMei1W9uyNo", 2), "KernelTimeGuestValue": (nid("KernelTimeGuestValue"), 1),
                      **{name: (name, 3) for name in THREADS},
                      **{name: (nid(name), 3) for name in KERNEL},
                      **{name: (nid(name), 4) for name in POSIX}}
    module.HOST_FILES = ("libkernel.prx",)
    return module


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("Usage: BuildKernelTimeFixture.py guest-linked.elf main-linked.elf guest.prx main.elf")
    module = packager()
    module.package(sys.argv[1], sys.argv[3], False)
    module.package(sys.argv[2], sys.argv[4], True)
