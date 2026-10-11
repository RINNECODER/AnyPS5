#!/usr/bin/env python3
"""Turn an ld.lld-linked x86-64 ELF into a PS5-style SCE image for the relinker.

usage: package_fixture.py <linked.elf> <output> --module NAME [--main]
                          [--import-lib LIB:sym[=func|obj|tls],...]...

Exports become NID#A#A. Each --import-lib gets the next library index (B, C,
...) and its symbols become NID#<index>#<index>; __tls_get_addr is imported by
its NID from the library that lists it. This is the general form of
core/cpu/tests/BuildSceModulesFixture.py without that fixture's fixed checks.
"""
import argparse
import base64
import hashlib
import struct
from pathlib import Path

NID_SUFFIX = bytes.fromhex("518d64a635ded8c1e6b039b1c3e55230")
RAW_NIDS = {"__tls_get_addr": "vNe1w4diLCs"}
ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"


def nid(name):
    if name in RAW_NIDS:
        return RAW_NIDS[name]
    digest = hashlib.sha1(name.encode("ascii") + NID_SUFFIX).digest()
    return base64.b64encode(digest[:8][::-1], altchars=b"+-").decode("ascii").rstrip("=")


def package(linked, output, module, main, import_libs):
    image = bytearray(Path(linked).read_bytes())
    if image[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<HH", image, 16) != (3, 62):
        raise SystemExit("fixture needs a linked little-endian x86-64 ET_DYN")
    phoff, shoff = struct.unpack_from("<QQ", image, 32)
    phsize, phcount, shsize, shcount = struct.unpack_from("<HHHH", image, 54)
    headers = [struct.unpack_from("<IIQQQQQQ", image, phoff + i * phsize) for i in range(phcount)]
    sections = [struct.unpack_from("<IIQQQQIIQQ", image, shoff + i * shsize) for i in range(shcount)]

    def virtual(address, size):
        for h in headers:
            if h[0] == 1 and h[3] <= address and address - h[3] + size <= h[5]:
                return bytes(image[h[2] + address - h[3]:h[2] + address - h[3] + size])
        raise SystemExit("linked table is not file-backed")

    dynamic = next(h for h in headers if h[0] == 2)
    tags, needed = {}, []
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack_from("<QQ", image, offset)
        if not tag:
            break
        if tag == 1:
            needed.append(value)
        else:
            tags[tag] = value
    old_strings = virtual(tags[5], tags[10])

    def old_string(offset):
        return old_strings[offset:old_strings.index(b"\0", offset)].decode("ascii")

    types = {"func": 2, "obj": 1, "tls": 6}
    library_of = {}
    for index, (_, symbols) in enumerate(import_libs, start=1):
        for symbol in symbols:
            name, _, kind = symbol.partition("=")
            library_of[name] = (index, types[kind or "func"])
    dynsym = next(s for s in sections if s[1] == 11)
    symbols = bytearray(image[dynsym[4]:dynsym[4] + dynsym[5]])
    strings = bytearray(b"\0")

    def new_string(value):
        offset = len(strings)
        strings.extend(value.encode("ascii") + b"\0")
        return offset

    for offset in range(24, len(symbols), 24):
        name_offset, info, _, section = struct.unpack_from("<IBBH", symbols, offset)
        name = old_string(name_offset)
        if section:
            scoped = nid(name) + "#A#A"
        else:
            if name not in library_of:
                raise SystemExit(f"{module}: import {name} is in no --import-lib")
            index, kind = library_of[name]
            letter = ALPHABET[index]
            scoped = f"{nid(name)}#{letter}#{letter}"
            symbols[offset + 4] = (info & 0xF0) | kind
        struct.pack_into("<I", symbols, offset, new_string(scoped))

    own = new_string(module)
    sce_tags = [(0x61000043, (0x101 << 32) | own), (0x61000047, (1 << 32) | own), (0x61000017, 0)]
    for index, (library, _) in enumerate(import_libs, start=1):
        name = new_string(library)
        sce_tags += [(0x61000045, (index << 48) | (0x101 << 32) | name),
                     (0x61000049, (index << 48) | (1 << 32) | name), (0x61000019, index << 48)]
    for value in needed:
        sce_tags.append((1, new_string(old_string(value))))
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
    rela_offset = append(rela)
    sce_tags += [(0x61000035, str_offset), (0x61000037, len(strings)), (0x61000039, sym_offset),
                 (0x6100003b, 24), (0x6100003f, len(symbols)), (0x6100002f, rela_offset),
                 (0x61000031, len(rela)), (0x61000033, 24)]
    if plt:
        sce_tags += [(0x61000029, append(plt)), (0x6100002d, len(plt)), (0x6100002b, 7)]
    if 3 in tags:
        sce_tags.append((0x61000027, tags[3]))
    replaced = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 20, 23, 0x6ffffef5}
    for tag, value in tags.items():
        if tag not in replaced and tag not in (0x6ffffff9, 0x6ffffffb, 0x6ffffffe, 0x6fffffff, 0x6ffffff0, 30):
            sce_tags.append((tag, value))
    sce_tags.append((0, 0))

    image.extend(b"\0" * (-len(image) % 8))
    data_offset = len(image)
    image.extend(data)
    image.extend(b"\0" * (-len(image) % 8))
    dynamic_offset = len(image)
    dynamic_data = b"".join(struct.pack("<QQ", *tag) for tag in sce_tags)
    image.extend(dynamic_data)
    kept = [h for h in headers if h[0] in (1, 7, 0x6474e551, 0x6474e552)]
    kept += [(2, 4, dynamic_offset, 0, 0, len(dynamic_data), len(dynamic_data), 8),
             (0x61000000, 4, data_offset, 0, 0, len(data), 0, 8)]
    image.extend(b"\0" * (-len(image) % 8))
    table_offset = len(image)
    image.extend(b"".join(struct.pack("<IIQQQQQQ", *h) for h in kept))
    image[7:9] = bytes((9, 2))
    struct.pack_into("<H", image, 16, 0xFE10 if main else 0xFE18)
    struct.pack_into("<Q", image, 32, table_offset)
    struct.pack_into("<Q", image, 40, 0)
    struct.pack_into("<H", image, 56, len(kept))
    struct.pack_into("<HHH", image, 58, 0, 0, 0)
    Path(output).write_bytes(image)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("linked")
    parser.add_argument("output")
    parser.add_argument("--module", required=True)
    parser.add_argument("--main", action="store_true")
    parser.add_argument("--import-lib", action="append", default=[])
    args = parser.parse_args()
    libs = []
    for spec in args.import_lib:
        library, _, names = spec.partition(":")
        libs.append((library, [name for name in names.split(",") if name]))
    package(args.linked, args.output, args.module, args.main, libs)


if __name__ == "__main__":
    main()
