import hashlib
import json
from pathlib import Path
import struct
import sys
from BuildSceCrtFixture import Elf, nid


RUNTIME_FILE = "SceMainLifecycleRuntime.prx"
RUNTIME_EXPORTS = {"SceMainLifecycleState": 1, "SceMainLifecycleDependencyInit": 2,
                   "SceMainLifecycleDependencyFini": 2, "8G2LB+A3rzg": 2, "uMei1W9uyNo": 2}
MAIN_EXPORTS = {"_start": 2, "SceMainLifecycleInit": 2, "SceMainLifecycleFini": 2,
                "SceMainLifecycleRegisteredTermination": 2, "SceMainLifecycleMain": 2}


def identity(name):
    return name if name in ("8G2LB+A3rzg", "uMei1W9uyNo", "6Z83sYWFlA8") else nid(name)


def package(linked_path, main):
    elf = Elf(Path(linked_path).read_bytes())
    image = bytearray(elf.image)
    shoff = struct.unpack_from("<Q", image, 40)[0]
    shsize, shcount = struct.unpack_from("<HH", image, 58)
    if shsize != 64:
        raise ValueError("Invalid lifecycle section header size")
    sections = [struct.unpack_from("<IIQQQQIIQQ", image, shoff + index * 64) for index in range(shcount)]
    dynamic_symbols, = [section for section in sections if section[1] == 11]
    if dynamic_symbols[9] != 24 or dynamic_symbols[5] % 24:
        raise ValueError("Invalid lifecycle dynamic symbol table")
    symbols = bytearray(elf.slice(dynamic_symbols[4], dynamic_symbols[5]))
    old_strings = elf.virtual(elf.tags[5], elf.tags[10])
    strings = bytearray(b"\0")

    def old_string(offset):
        end = old_strings.find(b"\0", offset)
        if offset >= len(old_strings) or end < 0:
            raise ValueError("Invalid lifecycle dynamic string")
        return old_strings[offset:end].decode("ascii")

    def string(value):
        offset = len(strings)
        strings.extend(value.encode("ascii") + b"\0")
        return offset

    definitions = MAIN_EXPORTS if main else RUNTIME_EXPORTS
    imports = {"SceMainLifecycleState": 1, "8G2LB+A3rzg": 2, "uMei1W9uyNo": 2} if main else {"6Z83sYWFlA8": 2}
    found_definitions, found_imports, addresses = set(), set(), {}
    for offset in range(24, len(symbols), 24):
        name_offset, info, visibility, section, value, size = struct.unpack_from("<IBBHQQ", symbols, offset)
        name = old_string(name_offset)
        if section:
            if definitions.get(name) != info & 15 or visibility != 3:
                raise ValueError(f"Unexpected lifecycle definition: {name}")
            found_definitions.add(name)
            addresses[name] = {"address": value, "size": size, "nid": identity(name)}
            scoped = identity(name) + "#A#A"
        else:
            if name not in imports or info & 15 not in (0, imports[name]):
                raise ValueError(f"Unexpected lifecycle import: {name}")
            found_imports.add(name)
            symbols[offset + 4] = (info & 0xf0) | imports[name]
            scoped = identity(name) + "#B#B"
        struct.pack_into("<I", symbols, offset, string(scoped))
    if found_definitions != set(definitions) or found_imports != set(imports):
        raise ValueError("Missing genuine compiled lifecycle imports/exports")
    needed = [old_string(offset) for offset in elf.needed]
    if needed != ([RUNTIME_FILE] if main else []):
        raise ValueError("Unexpected linked lifecycle dependencies")
    if not elf.tags.get(12) or not elf.tags.get(13) or any(elf.tags.get(tag) for tag in (25, 26, 27, 28, 32, 33)):
        raise ValueError("Lifecycle fixture requires scalar DT_INIT/DT_FINI and no implicit arrays")
    own = string("SceMainLifecycleMain" if main else "libc")
    imported = string("libc" if main else "libkernel")
    sce_tags = [(0x61000043, (0x101 << 32) | own), (0x61000047, (1 << 32) | own),
                (0x61000017, 0), (0x61000045, (1 << 48) | (0x101 << 32) | imported),
                (0x61000049, (1 << 48) | (1 << 32) | imported)]
    sce_tags += [(1, string(name)) for name in needed]
    if 14 in elf.tags:
        sce_tags.append((14, string(old_string(elf.tags[14]))))
    data = bytearray()

    def append(table):
        data.extend(b"\0" * (-len(data) % 8))
        offset = len(data)
        data.extend(table)
        return offset

    string_offset, symbol_offset = append(strings), append(symbols)
    sce_tags += [(0x61000035, string_offset), (0x61000037, len(strings)),
                 (0x61000039, symbol_offset), (0x6100003b, 24), (0x6100003f, len(symbols))]
    types = set()
    for address_tag, size_tag, sce_address, sce_size, sce_entry, entry in (
            (7, 8, 0x6100002f, 0x61000031, 0x61000033, 24),
            (23, 2, 0x61000029, 0x6100002d, 0x6100002b, 7)):
        if elf.tags.get(size_tag):
            table = elf.virtual(elf.tags[address_tag], elf.tags[size_tag])
            if len(table) % 24:
                raise ValueError("Invalid genuine lifecycle relocation table")
            types.update(info & 0xffffffff for _, info, _ in struct.iter_unpack("<QQq", table))
            sce_tags += [(sce_address, append(table)), (sce_size, len(table)), (sce_entry, entry)]
    if types != ({6, 7, 8} if main else {7}):
        raise ValueError(f"Missing genuine lifecycle object/PLT/constructor relocations: {types}")
    if 3 in elf.tags:
        sce_tags.append((0x61000027, elf.tags[3]))
    replaced = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 20, 23}
    allowed = {12, 13, 16, 21, 30, 0x6ffffef5, 0x6ffffff9, 0x6ffffffb}
    for tag, value in elf.tags.items():
        if tag in replaced:
            continue
        if tag not in allowed:
            raise ValueError(f"Unsupported lifecycle dynamic metadata: {tag:#x}")
        if tag != 0x6ffffef5:
            sce_tags.append((tag, value))
    sce_tags.append((0, 0))
    image.extend(b"\0" * (-len(image) % 8))
    data_offset = len(image)
    image.extend(data)
    dynamic_offset = len(image)
    dynamic_data = b"".join(struct.pack("<QQ", *tag) for tag in sce_tags)
    image.extend(dynamic_data)
    headers = [header for header in elf.headers if header[0] in (1, 0x6474e551)]
    headers += [(2, 4, dynamic_offset, 0, 0, len(dynamic_data), len(dynamic_data), 8),
                (0x61000000, 4, data_offset, 0, 0, len(data) + len(dynamic_data), 0, 8)]
    phoff = len(image)
    image.extend(b"".join(struct.pack("<IIQQQQQQ", *header) for header in headers))
    image[7:9] = bytes((9, 2))
    struct.pack_into("<H", image, 16, 0xfe10 if main else 0xfe18)
    struct.pack_into("<QQ", image, 32, phoff, 0)
    struct.pack_into("<HHHH", image, 56, len(headers), 0, 0, 0)
    return bytes(image), elf, addresses


def build(runtime_linked, main_linked, destination):
    output = Path(destination)
    output.mkdir(parents=True, exist_ok=True)
    runtime, runtime_elf, runtime_symbols = package(runtime_linked, False)
    main, main_elf, main_symbols = package(main_linked, True)
    receipts = []
    for kind, filename, source, linked, symbols in (
            ("runtime", RUNTIME_FILE, runtime, runtime_elf, runtime_symbols),
            ("main", "SceMainLifecycleMain.elf", main, main_elf, main_symbols)):
        (output / filename).write_bytes(source)
        receipts.append({"kind": kind, "path": filename, "sha256": hashlib.sha256(source).hexdigest(),
                         "size": len(source), "init": linked.tags[12], "fini": linked.tags[13],
                         "state_address": symbols.get("SceMainLifecycleState", {}).get("address", 0), "symbols": symbols})
    result = {"schema_version": 1, "sources": receipts,
              "expected_after_dependency_init": [1, 1, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0],
              "expected_before_exit": [12345, 1, 1, 1, 1, 1, 0, 0, 0, 2886, 0, 3, 0],
              "expected_at_kernel_exit": [12345678, 1, 1, 1, 1, 1, 1, 1, 1, 12120050, 0, 3, 1]}
    (output / "SceMainLifecycleReceipt.json").write_text(json.dumps(result, indent=2) + "\n")
    lines = [" ".join(str(receipt[field]) for field in ("kind", "path", "sha256", "size", "init", "fini", "state_address"))
             for receipt in receipts]
    (output / "SceMainLifecycleReceipt.txt").write_text("\n".join(lines) + "\n")
    print(json.dumps(result, separators=(",", ":")))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("Usage: SceMainLifecycleBuild.py runtime-linked.elf main-linked.elf output-directory")
    build(*sys.argv[1:])
