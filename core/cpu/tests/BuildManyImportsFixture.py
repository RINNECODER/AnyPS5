"""Generate, then package, a guest graph whose main image links more than a thousand imports.

The main image imports every libc service SceImports models under many library ids (one per
simulated consumer scope, the way each retail module numbers its own import libraries), so
the resolved imports alone need more host entry points than one gate page or the TCG engine's
256-gate budget holds, plus hundreds of NIDs no provider implements, which bind to trap stubs.
Packaging reuses the retained platform packager, like BuildLazyImportFixture.py.
"""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
helper = Path(__file__).resolve().parent / "BuildPlatformServiceFixture.py"
sys.path.insert(0, str(helper.parent))
from BuildSceModulesFixture import nid  # noqa: E402

ABSENT_MODULE = "libSceAnyPS5ManyAbsent"
# Scope ids are one character of the SCE NID alphabet, so 63 is the largest.
QUALIFIERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
LIBC_SERVICES = ("memcpy", "memmove", "memset", "strlen", "strcmp", "exit")
LIBC_SCOPES = range(4, 52)
UNKNOWN_COUNT = 800
SCOPES = {1: "ManyImportsGuest", 2: "libc", 3: ABSENT_MODULE, **{scope: "libc" for scope in LIBC_SCOPES}}


def resolved_name(service, scope):
    return f"anyps5_{service}_{scope:02d}"


def unknown_name(index):
    return f"anyps5ManyUnknown{index:04d}"


IMPORTS = {"ManyImportsGuestValue": (nid("ManyImportsGuestValue"), 1), "uMei1W9uyNo": ("uMei1W9uyNo", 2),
           **{resolved_name(service, scope): (nid(service), scope) for scope in LIBC_SCOPES for service in LIBC_SERVICES},
           **{unknown_name(index): (nid(unknown_name(index)), 3) for index in range(UNKNOWN_COUNT)}}


def source():
    lines = ["typedef unsigned long u64;", "typedef unsigned int u32;",
             '#define EXPORT __attribute__((visibility("protected")))', "", "#if BUILD_MANY_DEPENDENCY", "",
             "static volatile u32 anchor = 7;", "static volatile u32* volatile relocatedAnchor = &anchor;",
             "EXPORT u32 ManyImportsGuestValue(void) { return *relocatedAnchor; }", "", "#else", "",
             "extern u32 ManyImportsGuestValue(void);"]
    for scope in LIBC_SCOPES:
        lines += [f"extern void* {resolved_name('memcpy', scope)}(void*, const void*, u64);",
                  f"extern void* {resolved_name('memmove', scope)}(void*, const void*, u64);",
                  f"extern void* {resolved_name('memset', scope)}(void*, int, u64);",
                  f"extern u64 {resolved_name('strlen', scope)}(const char*);",
                  f"extern int {resolved_name('strcmp', scope)}(const char*, const char*);",
                  f"extern void {resolved_name('exit', scope)}(int);"]
    lines += [f"extern u64 {unknown_name(index)}(u64);" for index in range(UNKNOWN_COUNT)]
    lines += ["// Imported for their addresses only: exit would end the process.",
              "static void (*volatile exits[])(int) = {" +
              ", ".join(resolved_name("exit", scope) for scope in LIBC_SCOPES) + "};",
              "static const char text[] = \"anyps5 many imports\";",
              "static const char* volatile relocatedText = text;", "",
              "static int number(const char* value, u64* result) {",
              "    if (!value || !*value) return 0;", "    u64 parsed = 0, length = 0;",
              "    while (*value) {", "        if (++length > 10 || *value < '0' || *value > '9') return 0;",
              "        parsed = parsed * 10 + (unsigned)(*value++ - '0');", "    }",
              "    *result = parsed;", "    return 1;", "}", "",
              "EXPORT int SceGuestMain(u64 argc, char** argv) {",
              "    u64 expected;",
              "    if (argc != 2 || !argv[0] || argv[argc] || !number(argv[1], &expected)) return 81;",
              "    if (ManyImportsGuestValue() != 7) return 82;",
              "    for (unsigned index = 0; index < sizeof(exits) / sizeof(exits[0]); ++index) if (!exits[index]) return 83;",
              "    const char* source = relocatedText;",
              "    char first[32], second[32];"]
    # Every resolved import is called, so each one must reach its own host service.
    for scope in LIBC_SCOPES:
        lines += [f"    if ({resolved_name('memset', scope)}(first, {scope}, sizeof(first)) != first || "
                  f"first[31] != {scope}) return 84;",
                  f"    if ({resolved_name('memcpy', scope)}(first, source, 20) != first) return 85;",
                  f"    if ({resolved_name('memmove', scope)}(second, first, 20) != second) return 86;",
                  f"    if ({resolved_name('strlen', scope)}(second) != 19) return 87;",
                  f"    if ({resolved_name('strcmp', scope)}(second, source) != 0) return 88;"]
    # Every trap stub is called and must return the configured value.
    lines += [f"    if ({unknown_name(index)}({index}) != expected) return 80;" for index in range(UNKNOWN_COUNT)]
    lines += ["    return 0;", "}", "", "#endif", ""]
    return "\n".join(lines)


def packager():
    text = helper.read_text()
    replacements = {
        'expected_exports = {"_start", "SceGuestMain"} if main else {"PlatformServiceGuestMath"}':
            'expected_exports = {"_start", "SceGuestMain"} if main else {"ManyImportsGuestValue"}',
        'for filename in (GUEST_FILE, "libSceNpManager.prx", "libSceNet.prx", "libSceCommonDialog.prx"):':
            'for filename in (GUEST_FILE, *HOST_FILES):',
        # R_X86_64_64 stores the never-called exit imports' addresses.
        'types - {7, 8}:': 'types - {1, 7, 8}:',
        'qualifier = "ABCDEF"[scope]': 'qualifier = QUALIFIERS[scope]',
    }
    for old, new in replacements.items():
        if text.count(old) != 1:
            raise RuntimeError("Retained packager anchor changed: " + old)
        text = text.replace(old, new)
    spec = importlib.util.spec_from_file_location("many_imports_packager", helper)
    module = importlib.util.module_from_spec(spec)
    exec(compile(text, str(helper), "exec"), module.__dict__)
    module.GUEST_NAME = "ManyImportsGuest"
    module.GUEST_FILE = "ManyImportsGuest.prx"
    module.MAIN_NAME = "ManyImportsHomebrew"
    module.SCOPES = SCOPES
    module.QUALIFIERS = QUALIFIERS
    module.IMPORTS = IMPORTS
    module.HOST_FILES = (ABSENT_MODULE + ".prx",)
    return module


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "source":
        Path(sys.argv[2]).write_text(source())
    elif len(sys.argv) == 6 and sys.argv[1] == "package":
        module = packager()
        module.package(sys.argv[2], sys.argv[4], False)
        module.package(sys.argv[3], sys.argv[5], True)
    else:
        raise SystemExit("Usage: BuildManyImportsFixture.py source out.c | "
                         "package guest-linked.elf main-linked.elf guest.prx main.elf")
