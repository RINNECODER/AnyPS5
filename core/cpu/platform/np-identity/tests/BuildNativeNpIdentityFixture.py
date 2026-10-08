"""Package a real linked one-import public ELF; never provide API returns."""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
helper = Path(__file__).resolve().parents[2] / "tests/BuildScePlatformFixture.py"
spec = importlib.util.spec_from_file_location("np_fixture_packager", helper)
module = importlib.util.module_from_spec(spec)
source = helper.read_text()
replacements = {
    'for name, identity in (("libSceHttp",1),("libSceAjm",2)):':
        'for name, identity in sorted({(row[1], row[2]) for row in IMPORTS.values()}):',
    'len(plt) != 3*24': 'len(plt) != len(IMPORTS)*24',
    'PublicAbiFixtures Network/Audio only; no production ABI inference':
        'Native NP public fixture only; signed-out policy, no firmware identity layout claim',
}
for old, new in replacements.items():
    if source.count(old) != 1:
        raise ValueError(f"Retained public fixture packager changed: {old}")
    source = source.replace(old, new)
exec(compile(source, str(helper), "exec"), module.__dict__)
module.IMPORTS = {"sceNpGetOnlineId": ("XDncXQIJUSk", "libSceNpManager", 1, "B")}
if len(sys.argv) != 3:
    raise SystemExit("Usage: BuildNativeNpIdentityFixture.py linked.elf NativeNpIdentityGuest.elf")
module.package(*sys.argv[1:])
