"""One-import adapter; all actual linked symbol/relocation checks stay retained."""
import importlib.util
import hashlib
import json
import struct
from pathlib import Path
import sys

sys.dont_write_bytecode = True
helper = Path(__file__).resolve().parents[2] / "tests/BuildScePlatformFixture.py"
spec = importlib.util.spec_from_file_location("native_uri_fixture_helper", helper)
module = importlib.util.module_from_spec(spec)
source = helper.read_text()
replacements = {
    'for name, identity in (("libSceHttp",1),("libSceAjm",2)):':
        'for name, identity in sorted({(row[1], row[2]) for row in IMPORTS.values()}):',
    'len(plt) != 3*24': 'len(plt) != len(IMPORTS)*24',
    'PublicAbiFixtures Network/Audio only; no production ABI inference':
        'NativeUriGuest public fixture; consumer admission tested at typed native owner, no retail run',
}
for before, after in replacements.items():
    if source.count(before) != 1:
        raise RuntimeError(f"Retained packager adapter anchor changed: {before}")
    source = source.replace(before, after)
exec(compile(source, str(helper), "exec"), module.__dict__)
module.IMPORTS = {"sceHttpUriEscape": ("YuOW3dDAKYc", "libSceHttp", 1, "B")}
if len(sys.argv) != 3:
    raise SystemExit("Usage: BuildNativeUriFixture.py actual-linked.elf NativeUriGuest.elf")
module.package(*sys.argv[1:])
# Genuine alternative parser inputs protect immutable row/header corroboration.
# Each remains a real linked ELF with unchanged FUNC and relocation contracts.
linked, output = sys.argv[1:]
output = Path(output)
wrong_nid = output.parent / "uri-wrong-nid" / "NativeUriGuest.elf"
wrong_nid.parent.mkdir(parents=True, exist_ok=True)
module.IMPORTS = {"sceHttpUriEscape": ("AAAAAAAAAAA", "libSceHttp", 1, "B")}
module.package(linked, wrong_nid)
wrong_abi = output.parent / "uri-wrong-abi" / "NativeUriGuest.elf"
wrong_abi.parent.mkdir(parents=True, exist_ok=True)
image = bytearray(output.read_bytes())
image[7:9] = bytes((0, 0))
wrong_abi.write_bytes(image)
receipt = json.loads(output.with_suffix(".receipt.json").read_text())
receipt["sce_sha256"] = hashlib.sha256(image).hexdigest()
receipt["negative_contract"] = "Fresh ParseSce retains ABI 0/0; mutable top ABI 9/2 must not admit it"
wrong_abi.with_suffix(".receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")

wrong_library = output.parent / "uri-wrong-library" / "NativeUriGuest.elf"
wrong_library.parent.mkdir(parents=True, exist_ok=True)
image = bytearray(output.read_bytes())
phoff = struct.unpack_from("<Q", image, 32)[0]
phsize, phcount = struct.unpack_from("<HH", image, 54)
if phsize != 56:
    raise RuntimeError("Generated SCE fixture lost its ELF64 program header contract")
changed = 0
for index in range(phcount):
    header = struct.unpack_from("<IIQQQQQQ", image, phoff + index * phsize)
    if header[0] != 2:
        continue
    dynamic_offset, dynamic_size = header[2], header[5]
    for offset in range(dynamic_offset, dynamic_offset + dynamic_size, 16):
        tag, value = struct.unpack_from("<QQ", image, offset)
        if tag == 0:
            break
        if tag == 0x61000049 and value >> 48 == 1:
            if (value >> 32) & 0xffff != 1:
                raise RuntimeError("Healthy URI library version is not independently version1")
            # Change only the genuine library table version. Scoped symbol name,
            # module identity/version1.1, relocations and linked FUNC stay intact.
            struct.pack_into("<Q", image, offset + 8, (value & ~(0xffff << 32)) | (2 << 32))
            changed += 1
if changed != 1:
    raise RuntimeError("Generated SCE fixture lost its unique library ID1 version tag")
wrong_library.write_bytes(image)
receipt = json.loads(output.with_suffix(".receipt.json").read_text())
receipt["sce_sha256"] = hashlib.sha256(image).hexdigest()
for row in receipt["imports"]:
    row["library_version"] = 2
receipt["negative_contract"] = "Fresh ParseSce retains library version2 with unchanged URI NID/module1.1; mutable scope version1 must not admit it"
wrong_library.with_suffix(".receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
