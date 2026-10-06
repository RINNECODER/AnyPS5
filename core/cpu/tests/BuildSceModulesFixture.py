import base64
import hashlib
import math
from pathlib import Path
import struct
import sys
import zlib


GUEST_NAME = "SceModuleGuest"
MAIN_NAME = "SceModuleMain"
GUEST_FILE = "SceModuleGuest.prx"
EXIT_NID = "uMei1W9uyNo"
GUEST_EXPORTS = {
    "SceModuleInit": 2, "SceModuleFini": 2, "SceModuleMath": 2,
    "SceModuleObject": 1, "SceModuleInitCount": 1, "SceModuleFiniCount": 1,
    "SceModuleOrder": 1, "SceModuleTls": 6, "SceModuleTlsZero": 6,
    "SceModuleTlsPointer": 6, "SceModuleTlsIndex": 2,
}
MAIN_EXPORTS = {
    "_start": 2, "SceModuleMain": 2, "SceModuleMainInit": 2,
    "SceModuleMainInitCount": 1, "SceModuleMainState": 1,
    "SceModuleObjectAddress": 1, "SceModuleMainTls": 6,
    "SceModuleMainTlsPointer": 6, "SceModuleMainTlsZero": 6, "SceModuleImportedTlsIndex": 2,
    "SceModuleMainTlsIndex": 2,
}
MAIN_IMPORTS = {name: GUEST_EXPORTS[name] for name in (
    "SceModuleMath", "SceModuleObject", "SceModuleInitCount", "SceModuleFiniCount",
    "SceModuleOrder", "SceModuleTls", "SceModuleTlsZero")}
MAIN_IMPORTS[EXIT_NID] = 2


def nid(name):
    suffix = bytes.fromhex("518d64a635ded8c1e6b039b1c3e55230")
    digest = hashlib.sha1(name.encode("ascii") + suffix).digest()
    return base64.b64encode(digest[:8][::-1], altchars=b"+-").decode("ascii").rstrip("=")


def oracle(limit, seed):
    if not 2 <= limit <= 4095 or not 0 <= seed <= 255:
        raise ValueError("Module fixture arguments exceed supported input range")
    primes = [value for value in range(2, limit + 1)
              if all(value % divisor for divisor in range(2, math.isqrt(value) + 1))]
    pixels = bytes(((index * 37 + (index >> 3) + seed * 17) ^ (index >> 5)) & 255
                   for index in range(4096))
    return len(primes), sum(primes), zlib.adler32(pixels)


def expected_state(limit, seed):
    count, total, checksum = oracle(limit, seed)
    return (1, 1, 12, count, total, checksum, 0x2233445d + seed,
            0x31415927 + limit, 0x10203041 + limit, total + seed, count + seed)


def package(linked_path, output_path, main):
    image = bytearray(Path(linked_path).read_bytes())
    if image[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<HH", image, 16) != (3, 62):
        raise ValueError("Module fixture requires linker-produced little-endian x86-64 ET_DYN")
    phoff, = struct.unpack_from("<Q", image, 32)
    phsize, phcount = struct.unpack_from("<HH", image, 54)
    shoff, = struct.unpack_from("<Q", image, 40)
    shsize, shcount = struct.unpack_from("<HH", image, 58)
    if phsize != 56 or shsize != 64:
        raise ValueError("Unsupported linker header layout")

    def slice_bytes(offset, size):
        if offset > len(image) or size > len(image) - offset:
            raise ValueError("Linked module table exceeds file")
        return bytes(image[offset:offset + size])

    slice_bytes(phoff, phsize * phcount)
    slice_bytes(shoff, shsize * shcount)
    headers = [struct.unpack_from("<IIQQQQQQ", image, phoff + index * phsize)
               for index in range(phcount)]
    sections = [struct.unpack_from("<IIQQQQIIQQ", image, shoff + index * shsize)
                for index in range(shcount)]

    def virtual(address, size):
        for header in headers:
            if header[0] == 1 and address >= header[3] and address - header[3] + size <= header[5]:
                return slice_bytes(header[2] + address - header[3], size)
        raise ValueError("Linked module table is not file-backed")

    loads = [header for header in headers if header[0] == 1 and header[6]]
    for index, header in enumerate(loads):
        if header[1] not in (4, 5, 6) or header[5] > header[6]:
            raise ValueError("Module fixture requires valid readable LOAD permissions")
        start, end = header[3] & ~4095, (header[3] + header[6] + 4095) & ~4095
        for other in loads[:index]:
            if start < (other[3] + other[6] + 4095) & ~4095 and other[3] & ~4095 < end:
                raise ValueError("Link module fixture with separate guest pages")
    tls_headers = [header for header in headers if header[0] == 7]
    if len(tls_headers) != 1:
        raise ValueError("Module fixture must contain a real compiled TLS segment")
    tls = tls_headers[0]
    if not tls[5] or tls[6] <= tls[5] or tls[7] < 16 or tls[2] % tls[7] or tls[3] % tls[7]:
        raise ValueError("Module fixture needs aligned initialized and zero-fill TLS")
    virtual(tls[3], tls[5])
    if not any(header[1] & 4 and tls[3] >= header[3] and
               tls[3] - header[3] + tls[6] <= header[6] for header in loads):
        raise ValueError("Complete TLS allocation must lie in readable LOAD memory")
    for header in headers:
        if header[0] == 0x6474e552 and (header[3] % 4096 or header[6] % 4096):
            raise ValueError("Module fixture RELRO must use whole guest pages")

    dynamics = [header for header in headers if header[0] == 2]
    if len(dynamics) != 1 or dynamics[0][5] % 16:
        raise ValueError("Invalid linked dynamic segment")
    dynamic = dynamics[0]
    tags, needed = {}, []
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack("<QQ", slice_bytes(offset, 16))
        if not tag:
            break
        if tag == 1:
            needed.append(value)
        elif tag in tags:
            raise ValueError("Duplicate linked dynamic tag")
        else:
            tags[tag] = value
    else:
        raise ValueError("Unterminated linked dynamic table")
    old_strings = virtual(tags[5], tags[10])

    def old_string(offset):
        if offset >= len(old_strings):
            raise ValueError("Linked string offset exceeds table")
        end = old_strings.find(b"\0", offset)
        if end < 0:
            raise ValueError("Unterminated linked string")
        return old_strings[offset:end].decode("ascii")

    needed_names = [old_string(offset) for offset in needed]
    if needed_names != ([GUEST_FILE] if main else []):
        raise ValueError(f"Unexpected genuine linked dependencies: {needed_names}")
    if not tags.get(12) or (main and tags.get(13)) or (not main and not tags.get(13)):
        raise ValueError("Link dependency init/fini and main-owned init explicitly")
    if any(tags.get(tag) for tag in (25, 26, 27, 28, 32, 33)):
        raise ValueError("Module fixture lifecycle uses DT_INIT/DT_FINI, not implicit arrays")

    symbol_sections = [section for section in sections if section[1] == 11]
    if len(symbol_sections) != 1 or symbol_sections[0][9] != 24 or symbol_sections[0][5] % 24:
        raise ValueError("Invalid linked dynamic symbol table")
    dynsym = symbol_sections[0]
    symbols = bytearray(slice_bytes(dynsym[4], dynsym[5]))
    strings = bytearray(b"\0")

    def new_string(value):
        offset = len(strings)
        strings.extend(value.encode("ascii") + b"\0")
        return offset

    expected_exports = MAIN_EXPORTS if main else GUEST_EXPORTS
    expected_imports = MAIN_IMPORTS if main else {}
    exports, imports = set(), set()
    for offset in range(0, len(symbols), 24):
        name_offset, info, visibility, section, value, size = struct.unpack_from("<IBBHQQ", symbols, offset)
        name = old_string(name_offset)
        if not offset:
            if name or any(symbols[:24]):
                raise ValueError("Invalid linked null symbol")
            continue
        if not info >> 4:
            raise ValueError("Unexpected local dynamic symbol")
        if section:
            if name not in expected_exports or info & 15 != expected_exports[name] or visibility != 3:
                raise ValueError(f"Unexpected compiled module definition: {name}, type={info & 15}, visibility={visibility}")
            exports.add(name)
            scoped_name = nid(name) + "#A#A"
        else:
            if name not in expected_imports or (info & 15) not in (0, expected_imports[name]):
                raise ValueError(f"Unexpected compiled module import: {name}")
            imports.add(name)
            symbols[offset + 4] = (info & 0xf0) | expected_imports[name]
            scoped_name = EXIT_NID + "#C#C" if name == EXIT_NID else nid(name) + "#B#B"
        struct.pack_into("<I", symbols, offset, new_string(scoped_name))
    if exports != set(expected_exports) or imports != set(expected_imports):
        raise ValueError(f"Missing compiled exports/imports: {set(expected_exports) - exports}, {set(expected_imports) - imports}")

    own_name_offset = new_string(MAIN_NAME if main else GUEST_NAME)
    sce_tags = [(0x61000043, (0x101 << 32) | own_name_offset),
                (0x61000047, (1 << 32) | own_name_offset), (0x61000017, 0)]
    if main:
        guest_name_offset = new_string(GUEST_NAME)
        libc_name_offset = new_string("libc")
        for identity_id, name_offset in ((1, guest_name_offset), (2, libc_name_offset)):
            sce_tags += [(0x61000045, (identity_id << 48) | (0x101 << 32) | name_offset),
                         (0x61000049, (identity_id << 48) | (1 << 32) | name_offset),
                         (0x61000019, identity_id << 48)]
    for value in needed_names:
        sce_tags.append((1, new_string(value)))
    if 14 in tags:
        sce_tags.append((14, new_string(old_string(tags[14]))))

    data = bytearray()

    def append(table):
        data.extend(b"\0" * (-len(data) % 8))
        offset = len(data)
        data.extend(table)
        return offset

    str_offset = append(strings)
    sym_offset = append(symbols)
    rela = virtual(tags[7], tags[8]) if tags.get(8) else b""
    plt = virtual(tags[23], tags[2]) if tags.get(2) else b""
    if not rela or len(rela) % 24 or len(plt) % 24 or tags.get(9) != 24:
        raise ValueError("Module fixture requires genuine ELF64 RELA relocations")
    if main and (not plt or tags.get(20) != 7):
        raise ValueError("Main fixture requires a genuine cross-module PLT")
    types = {struct.unpack_from("<Q", table, offset + 8)[0] & 0xffffffff
             for table in (rela, plt) for offset in range(0, len(table), 24)}
    required_types = {1, 6, 7, 8, 16, 17, 18} if main else {8, 16, 18}
    if not required_types <= types or types - {1, 6, 7, 8, 16, 17, 18}:
        raise ValueError(f"Unexpected/missing genuine linker relocation types: {types}, required={required_types}")
    if not any(struct.unpack_from("<Q", rela, offset + 8)[0] & 0xffffffff == 8 and
               tls[3] <= struct.unpack_from("<Q", rela, offset)[0] <= tls[3] + tls[5] - 8
               for offset in range(0, len(rela), 24)):
        raise ValueError("Module fixture must relocate an actual pointer inside initialized TLS bytes")
    rela_offset = append(rela)
    sce_tags += [(0x61000035, str_offset), (0x61000037, len(strings)),
                 (0x61000039, sym_offset), (0x6100003b, 24), (0x6100003f, len(symbols)),
                 (0x6100002f, rela_offset), (0x61000031, len(rela)), (0x61000033, 24)]
    if plt:
        plt_offset = append(plt)
        sce_tags += [(0x61000029, plt_offset), (0x6100002d, len(plt)), (0x6100002b, 7)]
    if 3 in tags:
        sce_tags.append((0x61000027, tags[3]))
    replaced = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 20, 23, 0x6ffffef5}
    supported = {12, 13, 16, 21, 25, 26, 27, 28, 30, 32, 33, 0x6ffffff9, 0x6ffffffb}
    for tag, value in tags.items():
        if tag in replaced:
            continue
        if tag not in supported:
            raise ValueError(f"Unsupported linked dynamic metadata {tag:#x}")
        sce_tags.append((tag, value))
    sce_tags.append((0, 0))

    image.extend(b"\0" * (-len(image) % 8))
    data_offset = len(image)
    image.extend(data)
    image.extend(b"\0" * (-len(image) % 8))
    dynamic_offset = len(image)
    dynamic_data = b"".join(struct.pack("<QQ", *tag) for tag in sce_tags)
    image.extend(dynamic_data)
    kept = [header for header in headers if header[0] in (1, 7, 0x6474e551, 0x6474e552)]
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
    print(f"compiled SCE {'main' if main else 'PRX'}: {len(image)} bytes, "
          f"{len(imports)} imports, {len(exports)} exports, TLS {tls[5]}/{tls[6]} bytes, "
          f"relocation types {sorted(types)}, DT_INIT {tags[12]:#x}")


def package_pair(guest_linked, main_linked, guest_output, main_output):
    package(guest_linked, guest_output, False)
    package(main_linked, main_output, True)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("Usage: BuildSceModulesFixture.py guest-linked.elf main-linked.elf guest.prx main.elf")
    package_pair(*sys.argv[1:])
