import base64
import hashlib
import json
from pathlib import Path
import struct
import sys


class Elf:
    def __init__(self, image):
        self.image = image
        if image[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<H", image, 18)[0] != 62:
            raise ValueError("CRT fixture requires linked little-endian x86-64 ELF64")
        self.phoff = struct.unpack_from("<Q", image, 32)[0]
        phsize, count = struct.unpack_from("<HH", image, 54)
        if phsize != 56:
            raise ValueError("Unsupported program header size")
        self.headers = [struct.unpack_from("<IIQQQQQQ", image, self.phoff + index * 56) for index in range(count)]
        dynamic, = [header for header in self.headers if header[0] == 2]
        self.tags = {}
        self.needed = []
        for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
            tag, value = struct.unpack_from("<QQ", image, offset)
            if not tag:
                break
            if tag == 1:
                self.needed.append(value)
            elif tag in self.tags:
                raise ValueError("Duplicate linked dynamic tag")
            else:
                self.tags[tag] = value
        else:
            raise ValueError("Unterminated linked dynamic table")

    def virtual(self, address, size):
        for header in self.headers:
            if header[0] == 1 and address >= header[3] and address - header[3] + size <= header[5]:
                return self.slice(header[2] + address - header[3], size)
        raise ValueError("Linked CRT table is not file-backed")

    def slice(self, offset, size):
        if offset > len(self.image) or size > len(self.image) - offset:
            raise ValueError("CRT table exceeds linked file")
        return self.image[offset:offset + size]


def nid(name):
    digest = hashlib.sha1(name.encode("ascii") + bytes.fromhex("518d64a635ded8c1e6b039b1c3e55230")).digest()
    return base64.b64encode(digest[:8][::-1], altchars=b"+-").decode("ascii").rstrip("=")


def package(linked_path, main):
    elf = Elf(Path(linked_path).read_bytes())
    image = bytearray(elf.image)
    shoff = struct.unpack_from("<Q", image, 40)[0]
    shsize, shcount = struct.unpack_from("<HH", image, 58)
    if shsize != 64:
        raise ValueError("Unsupported section header size")
    sections = [struct.unpack_from("<IIQQQQIIQQ", image, shoff + index * 64) for index in range(shcount)]
    dynamic_symbols, = [section for section in sections if section[1] == 11]
    if dynamic_symbols[9] != 24 or dynamic_symbols[5] % 24:
        raise ValueError("Invalid CRT dynamic symbol table")
    symbols = bytearray(elf.slice(dynamic_symbols[4], dynamic_symbols[5]))
    old_strings = elf.virtual(elf.tags[5], elf.tags[10])
    strings = bytearray(b"\0")

    def string(value):
        offset = len(strings)
        strings.extend(value.encode("ascii") + b"\0")
        return offset

    def old_string(offset):
        end = old_strings.find(b"\0", offset)
        if offset >= len(old_strings) or end < 0:
            raise ValueError("Invalid linked CRT string")
        return old_strings[offset:end].decode("ascii")

    definitions = {"_start": 2} if main else {"SceCrtInit": 2, "SceCrtFini": 2, "SceCrtCheck": 2, "SceCrtState": 1}
    imports = {"SceCrtCheck": 2, "uMei1W9uyNo": 2} if main else {}
    found_definitions, found_imports, addresses = set(), set(), {}
    for offset in range(24, len(symbols), 24):
        name_offset, info, visibility, section, value, size = struct.unpack_from("<IBBHQQ", symbols, offset)
        name = old_string(name_offset)
        if section:
            if definitions.get(name) != info & 15 or visibility != 3:
                raise ValueError(f"Unexpected CRT definition: {name}")
            found_definitions.add(name)
            addresses[name] = {"address": value, "size": size, "nid": nid(name)}
            scoped = nid(name) + "#A#A"
        else:
            if name not in imports or info & 15 not in (0, 2):
                raise ValueError(f"Unexpected CRT import: {name}")
            found_imports.add(name)
            symbols[offset + 4] = (info & 0xf0) | 2
            scoped = "uMei1W9uyNo#C#C" if name == "uMei1W9uyNo" else nid(name) + "#B#B"
        struct.pack_into("<I", symbols, offset, string(scoped))
    if found_definitions != set(definitions) or found_imports != set(imports):
        raise ValueError("Missing genuine compiled CRT definitions/imports")
    needed = [old_string(offset) for offset in elf.needed]
    if needed != (["SceCrtGuest.prx"] if main else []):
        raise ValueError("Unexpected linked CRT dependencies")
    if main:
        if any(elf.tags.get(tag) for tag in (12, 13, 25, 26, 27, 28, 32, 33)):
            raise ValueError("CRT main must leave lifecycle ownership to the dependency")
    elif not elf.tags.get(12) or not elf.tags.get(13) or elf.tags.get(27) != 16 or elf.tags.get(28) != 16:
        raise ValueError("CRT dependency must have DT_INIT/DT_FINI and two genuine entries in each array")
    own = string("SceCrtMain" if main else "SceCrtGuest")
    sce_tags = [(0x61000043, (0x101 << 32) | own), (0x61000047, (1 << 32) | own), (0x61000017, 0)]
    if main:
        for local_id, name in ((1, "SceCrtGuest"), (2, "libc")):
            name_offset = string(name)
            sce_tags += [(0x61000045, (local_id << 48) | (0x101 << 32) | name_offset),
                         (0x61000049, (local_id << 48) | (1 << 32) | name_offset)]
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
    relocation_tables = []
    for address_tag, size_tag, sce_address, sce_size, sce_entry, entry in (
            (7, 8, 0x6100002f, 0x61000031, 0x61000033, 24),
            (23, 2, 0x61000029, 0x6100002d, 0x6100002b, 7)):
        if elf.tags.get(size_tag):
            table = elf.virtual(elf.tags[address_tag], elf.tags[size_tag])
            if len(table) % 24:
                raise ValueError("Invalid genuine CRT relocation table")
            relocation_tables.append(table)
            sce_tags += [(sce_address, append(table)), (sce_size, len(table)), (sce_entry, entry)]
    replaced = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 20, 23}
    allowed = {12, 13, 16, 21, 25, 26, 27, 28, 30, 32, 33, 0x6ffffff9, 0x6ffffffb}
    for tag, value in elf.tags.items():
        if tag in replaced:
            continue
        if tag not in allowed:
            raise ValueError(f"Unsupported linked CRT metadata: {tag:#x}")
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
    if not main:
        image.extend(bytes(index % 251 for index in range(1024 * 1024 + 37)))
    return bytes(image), elf, addresses, relocation_tables


def plaintext_self(source):
    elf = Elf(source)
    normalized = bytearray(source)
    table = b"".join(struct.pack("<IIQQQQQQ", *header) for header in elf.headers)
    if not any(header[0] == 1 and header[2] == 0 and header[5] >= 64 + len(table) for header in elf.headers):
        raise ValueError("CRT first LOAD must contain the normalized program headers")
    struct.pack_into("<Q", normalized, 32, 64)
    normalized[64:64 + len(table)] = table
    records = []
    for index, header in enumerate(elf.headers):
        if header[0] not in (1, 0x61000000) or not header[5]:
            continue
        payload = bytes(normalized[header[2]:header[2] + header[5]])
        blocks = [hashlib.sha256(payload[offset:offset + 16384]).digest() for offset in range(0, len(payload), 16384)]
        data_index = len(records) + 1
        records.append(((data_index << 20) | 0x10004, b"".join(blocks)))
        records.append(((index << 20) | 0x2804, payload))
    elf_base = 32 + len(records) * 32
    header_size = (elf_base + 64 + len(table) + 15) & ~15
    output = bytearray(header_size)
    output[elf_base:elf_base + 64 + len(table)] = normalized[:64 + len(table)]
    for index, (properties, payload) in enumerate(records):
        output.extend(b"\0" * (-len(output) % 16))
        offset = len(output)
        output.extend(payload)
        struct.pack_into("<QQQQ", output, 32 + index * 32, properties, offset, len(payload), len(payload))
    struct.pack_into("<IBBBBIHHQHHI", output, 0, 0xeef51454, 0, 1, 1, 0x12, 0x101,
                     header_size, 0, len(output), len(records), 0x22, 0)
    return bytes(output)


def build(guest_linked, main_linked, destination):
    output = Path(destination)
    (output / "raw").mkdir(parents=True, exist_ok=True)
    (output / "plain-self").mkdir(exist_ok=True)
    guest, linked, symbols, relocations = package(guest_linked, False)
    main, _, _, _ = package(main_linked, True)
    container = plaintext_self(guest)
    (output / "sce-crt-main.elf").write_bytes(main)
    (output / "raw/SceCrtGuest.prx").write_bytes(guest)
    (output / "plain-self/SceCrtGuest.prx").write_bytes(container)
    tags = linked.tags
    arrays = {}
    for name, address_tag, size_tag, owner in (("preinit", 32, 33, "unsupported"),
                                              ("init_array", 25, 27, "dt_init"),
                                              ("fini_array", 26, 28, "dt_fini")):
        address, size = tags.get(address_tag, 0), tags.get(size_tag, 0)
        entries = []
        for slot in range(address, address + size, 8):
            matches = [addend for table in relocations for offset, info, addend in struct.iter_unpack("<QQq", table)
                       if offset == slot and info & 0xffffffff == 8]
            if len(matches) != 1:
                raise ValueError("Each CRT array slot must retain one real linker RELATIVE relocation")
            entries.append(matches[0])
        arrays[name] = {"address": address, "size": size, "owner": owner, "entries": entries}
    receipts = []
    for kind, path, source in (("elf", "raw/SceCrtGuest.prx", guest),
                               ("plain_self", "plain-self/SceCrtGuest.prx", container)):
        receipts.append({"kind": kind, "path": path, "sha256": hashlib.sha256(source).hexdigest(),
                         "size": len(source), "init": tags[12], "fini": tags[13],
                         "arrays": arrays, "state_address": symbols["SceCrtState"]["address"]})
    result = {"schema_version": 1, "sources": receipts, "symbols": symbols,
              "expected_after_init": [123, 1, 0, 1, 1, 0, 0, 82, 0],
              "expected_after_main": [1234, 1, 0, 1, 1, 0, 0, 577, 1],
              "expected_after_fini": [1234576, 1, 1, 1, 1, 1, 1, 82583, 1]}
    (output / "crt-receipt.json").write_text(json.dumps(result, indent=2) + "\n")
    lines = []
    for receipt in receipts:
        fields = [receipt[name] for name in ("kind", "path", "sha256", "size", "init", "fini")]
        for name in ("preinit", "init_array", "fini_array"):
            fields.extend((arrays[name]["address"], arrays[name]["size"]))
        fields.append(receipt["state_address"])
        lines.append(" ".join(map(str, fields)))
    (output / "crt-receipt.txt").write_text("\n".join(lines) + "\n")
    print(json.dumps(result, separators=(",", ":")))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("Usage: BuildSceCrtFixture.py guest-linked.elf main-linked.elf output-directory")
    build(*sys.argv[1:])
