from pathlib import Path
import struct
import sys

from BuildSceModulesFixture import nid


GUEST_NAME = "PlatformServiceGuest"
GUEST_FILE = GUEST_NAME + ".prx"
MAIN_NAME = "PlatformServiceHomebrew"
SCOPES = {1: GUEST_NAME, 2: "libc", 3: "libSceNpManager", 4: "libSceNet", 5: "libSceCommonDialog"}
IMPORTS = {
    "PlatformServiceGuestMath": (nid("PlatformServiceGuestMath"), 1),
    "uMei1W9uyNo": ("uMei1W9uyNo", 2),
    "eQH7nWPcAgc": ("eQH7nWPcAgc", 3),
    "9T2pDF2Ryqg": ("9T2pDF2Ryqg", 4),
    "iWQWrwiSt8A": ("iWQWrwiSt8A", 4),
    "8Kcp5d-q1Uo": ("8Kcp5d-q1Uo", 4),
    "9vA2aW+CHuA": ("9vA2aW+CHuA", 4),
    "uoUpLGNkygk": ("uoUpLGNkygk", 5),
}


def package(linked_path, output_path, main):
    image = bytearray(Path(linked_path).read_bytes())

    def read(offset, size):
        if offset > len(image) or size > len(image) - offset:
            raise ValueError("Linked platform fixture table exceeds file")
        return bytes(image[offset:offset + size])

    if read(0, 7) != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<HH", read(16, 4)) != (3, 62):
        raise ValueError("Platform fixture requires linker-produced little-endian x86-64 ET_DYN")
    phoff, = struct.unpack_from("<Q", image, 32)
    shoff, = struct.unpack_from("<Q", image, 40)
    phsize, phcount, shsize, shcount = struct.unpack_from("<HHHH", image, 54)
    if phsize != 56 or shsize != 64:
        raise ValueError("Unsupported linked platform header layout")
    headers = [struct.unpack("<IIQQQQQQ", read(phoff + index * phsize, phsize))
               for index in range(phcount)]
    sections = [struct.unpack("<IIQQQQIIQQ", read(shoff + index * shsize, shsize))
                for index in range(shcount)]
    loads = [header for header in headers if header[0] == 1 and header[6]]
    for index, header in enumerate(loads):
        if header[1] not in (4, 5, 6) or header[5] > header[6]:
            raise ValueError("Platform fixture needs valid readable LOAD segments")
        start, end = header[3] & ~4095, (header[3] + header[6] + 4095) & ~4095
        for other in loads[:index]:
            if start < ((other[3] + other[6] + 4095) & ~4095) and (other[3] & ~4095) < end:
                raise ValueError("Link platform fixture with separate permission pages")

    def virtual(address, size):
        for header in loads:
            if address >= header[3] and address - header[3] <= header[5] and size <= header[5] - (address - header[3]):
                return read(header[2] + address - header[3], size)
        raise ValueError("Linked platform table is not file-backed")

    dynamics = [header for header in headers if header[0] == 2]
    symbols_sections = [section for section in sections if section[1] == 11]
    if len(dynamics) != 1 or dynamics[0][5] % 16 or len(symbols_sections) != 1:
        raise ValueError("Platform fixture requires one bounded dynamic and symbol table")
    tags, needed = {}, []
    dynamic = dynamics[0]
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack("<QQ", read(offset, 16))
        if not tag:
            break
        if tag == 1:
            needed.append(value)
        elif tag in tags:
            raise ValueError("Duplicate linked platform dynamic tag")
        else:
            tags[tag] = value
    else:
        raise ValueError("Unterminated linked platform dynamic table")
    old_strings = virtual(tags[5], tags[10])

    def old_string(offset):
        if offset >= len(old_strings) or (end := old_strings.find(b"\0", offset)) < 0:
            raise ValueError("Invalid linked platform string")
        return old_strings[offset:end].decode("ascii")

    if [old_string(offset) for offset in needed] != ([GUEST_FILE] if main else []):
        raise ValueError("Platform fixture must genuinely link its guest dependency only")
    if any(tags.get(tag) for tag in (12, 13, 25, 26, 27, 28, 32, 33)) or any(header[0] == 7 for header in headers):
        raise ValueError("Platform fixture uses no implicit CRT arrays or TLS")
    dynsym = symbols_sections[0]
    if dynsym[9] != 24 or dynsym[5] % 24:
        raise ValueError("Invalid linked platform symbol entries")
    symbols = bytearray(read(dynsym[4], dynsym[5]))
    strings = bytearray(b"\0")

    def new_string(value):
        offset = len(strings)
        strings.extend(value.encode("ascii") + b"\0")
        return offset

    expected_exports = {"_start", "SceGuestMain"} if main else {"PlatformServiceGuestMath"}
    expected_imports = IMPORTS if main else {}
    exports, imports = set(), set()
    for offset in range(0, len(symbols), 24):
        name_offset, info, visibility, section, value, size = struct.unpack_from("<IBBHQQ", symbols, offset)
        name = old_string(name_offset)
        if not offset:
            if name or any(symbols[:24]):
                raise ValueError("Invalid linked platform null symbol")
            continue
        if not info >> 4:
            raise ValueError("Unexpected local platform dynamic symbol")
        if section:
            if name not in expected_exports or info & 15 != 2 or visibility not in (0, 3):
                raise ValueError(f"Unexpected platform definition: {name}")
            exports.add(name)
            qualified = nid(name) + "#A#A"
        else:
            if name not in expected_imports or info & 15 not in (0, 2):
                raise ValueError(f"Unexpected platform import: {name}")
            imports.add(name)
            import_nid, scope = expected_imports[name]
            qualifier = "ABCDEF"[scope]
            qualified = import_nid + "#" + qualifier + "#" + qualifier
            symbols[offset + 4] = (info & 0xf0) | 2
        struct.pack_into("<I", symbols, offset, new_string(qualified))
    if exports != expected_exports or imports != set(expected_imports):
        raise ValueError("Compiled platform fixture lost a required definition or service call")

    own_offset = new_string(MAIN_NAME if main else GUEST_NAME)
    sce_tags = [(0x61000043, (0x101 << 32) | own_offset),
                (0x61000047, (1 << 32) | own_offset), (0x61000017, 0)]
    if main:
        for scope, name in SCOPES.items():
            name_offset = new_string(name)
            sce_tags += [(0x61000045, (scope << 48) | (0x101 << 32) | name_offset),
                         (0x61000049, (scope << 48) | (1 << 32) | name_offset),
                         (0x61000019, scope << 48)]
        for filename in (GUEST_FILE, "libSceNpManager.prx", "libSceNet.prx", "libSceCommonDialog.prx"):
            sce_tags.append((1, new_string(filename)))
    if 14 in tags:
        sce_tags.append((14, new_string(old_string(tags[14]))))

    data = bytearray()

    def append(table):
        data.extend(b"\0" * (-len(data) % 8))
        offset = len(data)
        data.extend(table)
        return offset

    str_offset, sym_offset = append(strings), append(symbols)
    rela = virtual(tags[7], tags[8]) if tags.get(8) else b""
    plt = virtual(tags[23], tags[2]) if tags.get(2) else b""
    if not rela or len(rela) % 24 or len(plt) % 24 or tags.get(9) != 24:
        raise ValueError("Platform fixture needs genuine linker RELA entries")
    if main and (not plt or tags.get(20) != 7):
        raise ValueError("Platform main needs actual cross-module PLT calls")
    types = {struct.unpack_from("<Q", table, offset + 8)[0] & 0xffffffff
             for table in (rela, plt) for offset in range(0, len(table), 24)}
    if 8 not in types or (main and 7 not in types) or types - {7, 8}:
        raise ValueError(f"Unexpected platform linker relocation types: {types}")
    sce_tags += [(0x61000035, str_offset), (0x61000037, len(strings)),
                 (0x61000039, sym_offset), (0x6100003b, 24), (0x6100003f, len(symbols)),
                 (0x6100002f, append(rela)), (0x61000031, len(rela)), (0x61000033, 24)]
    if plt:
        sce_tags += [(0x61000029, append(plt)), (0x6100002d, len(plt)), (0x6100002b, 7)]
    if 3 in tags:
        sce_tags.append((0x61000027, tags[3]))
    replaced = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 20, 23, 0x6ffffef5}
    for tag, value in tags.items():
        if tag in replaced:
            continue
        if tag not in {16, 21, 30, 0x6ffffff9, 0x6ffffffb}:
            raise ValueError(f"Unsupported linked platform dynamic tag {tag:#x}")
        sce_tags.append((tag, value))
    sce_tags.append((0, 0))
    image.extend(b"\0" * (-len(image) % 8))
    data_offset = len(image)
    image.extend(data)
    image.extend(b"\0" * (-len(image) % 8))
    dynamic_offset = len(image)
    dynamic_data = b"".join(struct.pack("<QQ", *tag) for tag in sce_tags)
    image.extend(dynamic_data)
    kept = [header for header in headers if header[0] in (1, 0x6474e551)]
    kept += [(2, 4, dynamic_offset, 0, 0, len(dynamic_data), len(dynamic_data), 8),
             (0x61000000, 4, data_offset, 0, 0, len(data), 0, 8)]
    image.extend(b"\0" * (-len(image) % 8))
    table_offset = len(image)
    image.extend(b"".join(struct.pack("<IIQQQQQQ", *header) for header in kept))
    image[7:9] = bytes((9, 2))
    struct.pack_into("<H", image, 16, 0xfe10 if main else 0xfe18)
    struct.pack_into("<Q", image, 32, table_offset)
    struct.pack_into("<Q", image, 40, 0)
    struct.pack_into("<H", image, 56, len(kept))
    struct.pack_into("<HHH", image, 58, 0, 0, 0)
    Path(output_path).write_bytes(image)
    print(f"Compiled platform {'main' if main else 'PRX'}: {len(imports)} typed imports, "
          f"{len(exports)} exports, genuine relocations {sorted(types)}")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("Usage: BuildPlatformServiceFixture.py guest-linked.elf main-linked.elf guest.prx main.elf")
    package(sys.argv[1], sys.argv[3], False)
    package(sys.argv[2], sys.argv[4], True)
