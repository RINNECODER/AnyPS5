import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tests"))
from BuildSceModulesFixture import nid


GUEST_EXPORTS = {"ThreadDependencyTls": 6, "ThreadDependencyPointer": 6,
                 "ThreadDependencyZero": 6, "ThreadDependencyAddress": 2, "ThreadDependencyProbe": 2,
                 "ThreadLifecycle": 1, "ThreadDependencyInit": 2, "ThreadDependencyFini": 2}
MAIN_EXPORTS = {"_start": 2, "SceGuestMain": 2, "ThreadMainTls": 6, "ThreadMainPointer": 6,
                "ThreadMainZero": 6, "ThreadMainGd": 6, "ThreadReceipt": 1, "ThreadEvents": 1,
                "ThreadCount": 2, "ThreadReport": 2, "ThreadDestructor": 2, "ThreadChild": 2, "ThreadArithmetic": 2}
THREAD_NIDS = {"rNhWz+lvOMU", "pB-yGZ2nQ9o", "WhCc1w3EhSI", "6UgtwV+0zb4", "T72hz6ffq08",
               "onNY9Byn-W8", "aI+OeCz8xrQ", "3PtV6p3QNX4", "9BcDykPmo1I", "3kg7rT0NQIs"}
MAIN_IMPORTS = {name: (nid(name), 1, symbol_type) for name, symbol_type in GUEST_EXPORTS.items()
                if name not in {"ThreadDependencyInit", "ThreadDependencyFini"}}
MAIN_IMPORTS.update({name: (name, 3, 2) for name in THREAD_NIDS})
MAIN_IMPORTS.update({"__tls_get_addr": ("vNe1w4diLCs", 3, 2), "uMei1W9uyNo": ("uMei1W9uyNo", 2, 2)})
SCOPES = {1: "ThreadGuest", 2: "libc", 3: "libkernel"}


def package(linked, output, main):
    image = bytearray(Path(linked).read_bytes())

    def file_bytes(offset, count):
        if offset > len(image) or count > len(image) - offset:
            raise ValueError("Thread fixture ELF table exceeds the linked file")
        return bytes(image[offset:offset + count])

    if file_bytes(0, 7) != b"\x7fELF\x02\x01\x01" or struct.unpack("<HH", file_bytes(16, 4)) != (3, 62):
        raise ValueError("Thread fixture needs a genuine linked little-endian x86-64 shared ELF")
    phoff, = struct.unpack("<Q", file_bytes(32, 8))
    shoff, = struct.unpack("<Q", file_bytes(40, 8))
    phsize, phcount, shsize, shcount = struct.unpack("<HHHH", file_bytes(54, 8))
    if (phsize, shsize) != (56, 64):
        raise ValueError("Unsupported thread fixture ELF table sizes")
    headers = [struct.unpack("<IIQQQQQQ", file_bytes(phoff + index * phsize, phsize))
               for index in range(phcount)]
    sections = [struct.unpack("<IIQQQQIIQQ", file_bytes(shoff + index * shsize, shsize))
                for index in range(shcount)]
    loads = [header for header in headers if header[0] == 1 and header[6]]
    for index, header in enumerate(loads):
        if header[1] not in (4, 5, 6) or header[5] > header[6]:
            raise ValueError("Invalid thread fixture LOAD permissions or extent")
        begin, end = header[3] & ~4095, (header[3] + header[6] + 4095) & ~4095
        for other in loads[:index]:
            if begin < ((other[3] + other[6] + 4095) & ~4095) and (other[3] & ~4095) < end:
                raise ValueError("Thread fixture LOADs must have separate permission pages")

    def virtual(address, count):
        for header in loads:
            if address >= header[3] and address - header[3] <= header[5] and count <= header[5] - (address - header[3]):
                return file_bytes(header[2] + address - header[3], count)
        raise ValueError("Thread fixture table is not backed by linked LOAD bytes")

    tls_headers = [header for header in headers if header[0] == 7]
    if len(tls_headers) != 1:
        raise ValueError("Thread fixture requires one compiler-generated TLS segment")
    tls = tls_headers[0]
    if not tls[5] or tls[6] <= tls[5] or tls[7] < 16 or tls[3] % tls[7]:
        raise ValueError("Thread fixture needs aligned initialized TLS and zero-fill BSS")
    virtual(tls[3], tls[5])
    dynamic_headers = [header for header in headers if header[0] == 2]
    if len(dynamic_headers) != 1 or dynamic_headers[0][5] % 16:
        raise ValueError("Thread fixture requires one bounded ELF dynamic table")
    dynamic = dynamic_headers[0]
    tags, needed = {}, []
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack("<QQ", file_bytes(offset, 16))
        if not tag:
            break
        if tag == 1:
            needed.append(value)
        elif tag in tags:
            raise ValueError("Duplicate linked thread dynamic tag")
        else:
            tags[tag] = value
    else:
        raise ValueError("Unterminated linked thread dynamic table")
    original_strings = virtual(tags[5], tags[10])

    def original_string(offset):
        if offset >= len(original_strings) or (end := original_strings.find(b"\0", offset)) < 0:
            raise ValueError("Invalid linked thread string")
        return original_strings[offset:end].decode("ascii")

    if [original_string(offset) for offset in needed] != (["ThreadGuest.prx"] if main else []):
        raise ValueError("Thread main must genuinely link only its guest dependency")
    if any(tags.get(tag) for tag in (25, 26, 27, 28, 32, 33)):
        raise ValueError("Thread fixture does not use implicit constructor or finalizer arrays")
    if main and (tags.get(12) or tags.get(13)):
        raise ValueError("Thread main must enter through its genuine startup entry")
    if not main and (not tags.get(12) or not tags.get(13)):
        raise ValueError("Thread dependency needs actual linked DT_INIT and DT_FINI")
    symbol_sections = [section for section in sections if section[1] == 11]
    if len(symbol_sections) != 1 or symbol_sections[0][9] != 24 or symbol_sections[0][5] % 24:
        raise ValueError("Invalid linked thread symbol table")
    symbols = bytearray(file_bytes(symbol_sections[0][4], symbol_sections[0][5]))
    strings = bytearray(b"\0")

    def string(value):
        offset = len(strings)
        strings.extend(value.encode("ascii") + b"\0")
        return offset

    exports = MAIN_EXPORTS if main else GUEST_EXPORTS
    imports = MAIN_IMPORTS if main else {"__tls_get_addr": ("vNe1w4diLCs", 3, 2),
                                       "aI+OeCz8xrQ": ("aI+OeCz8xrQ", 3, 2),
                                       "9BcDykPmo1I": ("9BcDykPmo1I", 3, 2)}
    found_exports, found_imports, binding = set(), set(), []
    for offset in range(0, len(symbols), 24):
        old_name, info, visibility, section, value, size = struct.unpack_from("<IBBHQQ", symbols, offset)
        name = original_string(old_name)
        if not offset:
            if name or any(symbols[:24]):
                raise ValueError("Invalid linked thread null symbol")
            continue
        if not info >> 4:
            raise ValueError("Unexpected local linked thread dynamic symbol")
        if section:
            if name not in exports or info & 15 != exports[name] or visibility not in (0, 3):
                raise ValueError(f"Unexpected compiled thread definition: {name}")
            found_exports.add(name)
            qualified = nid(name) + "#A#A"
        else:
            if name not in imports or info & 15 not in (0, imports[name][2]):
                raise ValueError(f"Unexpected compiled thread import: {name}")
            found_imports.add(name)
            import_nid, scope, kind = imports[name]
            symbols[offset + 4] = (info & 0xf0) | kind
            letter = "ABCD"[scope]
            qualified = import_nid + "#" + letter + "#" + letter
        binding.append({"name": name, "qualified": qualified, "type": symbols[offset + 4] & 15,
                        "defined": bool(section), "value": value, "size": size})
        struct.pack_into("<I", symbols, offset, string(qualified))
    if found_exports != set(exports) or found_imports != set(imports):
        raise ValueError(f"Missing compiled thread symbols: {set(exports)-found_exports}, {set(imports)-found_imports}")
    if not main:
        for tag, function in ((12, "ThreadDependencyInit"), (13, "ThreadDependencyFini")):
            if tags[tag] != next(item["value"] for item in binding if item["name"] == function):
                raise ValueError("Linked lifecycle tag does not address its compiled exported function")
    own = string("ThreadMain" if main else "ThreadGuest")
    sce_tags = [(0x61000043, (0x101 << 32) | own), (0x61000047, (1 << 32) | own), (0x61000017, 0)]
    for scope in ((1, 2, 3) if main else (3,)):
        name = string(SCOPES[scope])
        sce_tags += [(0x61000045, (scope << 48) | (0x101 << 32) | name),
                     (0x61000049, (scope << 48) | (1 << 32) | name), (0x61000019, scope << 48)]
    for filename in (["ThreadGuest.prx", "libkernel.prx"] if main else ["libkernel.prx"]):
        sce_tags.append((1, string(filename)))
    if 14 in tags:
        sce_tags.append((14, string(original_string(tags[14]))))
    data = bytearray()

    def append(table):
        data.extend(b"\0" * (-len(data) % 8))
        offset = len(data)
        data.extend(table)
        return offset

    str_offset, sym_offset = append(strings), append(symbols)
    rela = virtual(tags[7], tags[8]) if tags.get(8) else b""
    plt = virtual(tags[23], tags[2]) if tags.get(2) else b""
    if not rela or not plt or len(rela) % 24 or len(plt) % 24 or tags.get(9) != 24 or tags.get(20) != 7:
        raise ValueError("Thread fixture needs genuine ELF64 TLS/RELATIVE and PLT relocations")
    relocations = [struct.unpack_from("<QQq", table, offset) for table in (rela, plt) for offset in range(0, len(table), 24)]
    kinds = {info & 0xffffffff for target, info, addend in relocations}
    if not {7, 8, 16} <= kinds or (main and 18 not in kinds) or kinds - {1, 6, 7, 8, 16, 17, 18}:
        raise ValueError(f"Missing actual thread TLS/PLT relocation kind: {kinds}")
    if not any(info & 0xffffffff == 8 and tls[3] <= target < tls[3] + tls[5] for target, info, addend in relocations):
        raise ValueError("Thread fixture must relocate a real pointer inside initialized TLS")
    sce_tags += [(0x61000035, str_offset), (0x61000037, len(strings)),
                 (0x61000039, sym_offset), (0x6100003b, 24), (0x6100003f, len(symbols)),
                 (0x6100002f, append(rela)), (0x61000031, len(rela)), (0x61000033, 24),
                 (0x61000029, append(plt)), (0x6100002d, len(plt)), (0x6100002b, 7)]
    if 3 in tags:
        sce_tags.append((0x61000027, tags[3]))
    replaced = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 20, 23, 0x6ffffef5}
    for tag, value in tags.items():
        if tag in replaced:
            continue
        if tag not in {12, 13, 16, 21, 30, 0x6ffffff9, 0x6ffffffb}:
            raise ValueError(f"Unsupported linked thread dynamic tag: {tag:#x}")
        sce_tags.append((tag, value))
    sce_tags.append((0, 0))
    image.extend(b"\0" * (-len(image) % 8))
    data_offset = len(image)
    image.extend(data)
    image.extend(b"\0" * (-len(image) % 8))
    dynamic_offset = len(image)
    dynamic_bytes = b"".join(struct.pack("<QQ", *tag) for tag in sce_tags)
    image.extend(dynamic_bytes)
    kept = [header for header in headers if header[0] in (1, 7, 0x6474e551)]
    kept += [(2, 4, dynamic_offset, 0, 0, len(dynamic_bytes), len(dynamic_bytes), 8),
             (0x61000000, 4, data_offset, 0, 0, len(data), 0, 8)]
    image.extend(b"\0" * (-len(image) % 8))
    header_offset = len(image)
    image.extend(b"".join(struct.pack("<IIQQQQQQ", *header) for header in kept))
    image[7:9] = bytes((9, 2))
    struct.pack_into("<H", image, 16, 0xfe10 if main else 0xfe18)
    struct.pack_into("<Q", image, 32, header_offset)
    struct.pack_into("<Q", image, 40, 0)
    struct.pack_into("<H", image, 56, len(kept))
    struct.pack_into("<HHH", image, 58, 0, 0, 0)
    Path(output).write_bytes(image)
    return {"file": Path(output).name, "sha256": hashlib.sha256(image).hexdigest(), "size": len(image),
            "linked_sha256": hashlib.sha256(Path(linked).read_bytes()).hexdigest(), "entry": struct.unpack_from("<Q", image, 24)[0],
            "tls_file_size": tls[5], "tls_memory_size": tls[6], "tls_alignment": tls[7],
            "dt_init": tags.get(12, 0), "dt_fini": tags.get(13, 0),
            "relocation_types": sorted(kinds), "bindings": binding,
            "load_bytes_sha256": [hashlib.sha256(file_bytes(header[2], header[5])).hexdigest() for header in loads]}


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("Usage: BuildThreadFixture.py guest-linked.elf main-linked.elf guest.prx main.elf")
    guest = package(sys.argv[1], sys.argv[3], False)
    main = package(sys.argv[2], sys.argv[4], True)
    receipt = {"schema_version": 1, "source_sha256": hashlib.sha256(Path(__file__).with_name("ThreadHomebrew.c").read_bytes()).hexdigest(),
               "builder_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "guest": guest, "main": main,
               "independent_arithmetic_result": 33644544, "events": [1, 2, 3, 4, 5, 6], "thread_return": "0x8877665544332211"}
    Path(sys.argv[4]).with_name("thread-fixture-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Compiled thread main: {len(MAIN_IMPORTS)} typed imports; real TLS/RELA {main['relocation_types']}; guest TLS/RELA {guest['relocation_types']}")
