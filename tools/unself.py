#!/usr/bin/env python3
"""Rebuild a plain ELF from a plaintext (unencrypted, uncompressed) SELF container.

The relinker only accepts ELF input. This mirrors DecodePlainSelf in
core/cpu/src/Self.cpp (same layout rules, fewer checks) so a SELF that the
TCG runtime loads can be fed to `relinker --macos --to-rosetta`.

usage: unself.py <input.self> <output.elf>
"""
import struct
import sys


def unself(data: bytes) -> bytes:
    if data[:4] not in (b"\x4f\x15\x3d\x1d", b"\x54\x14\xf5\xee"):
        if data[:4] == b"\x7fELF":
            return data
        raise SystemExit("not a SELF container")
    header_size, meta_size, file_size, count = struct.unpack_from("<HHQH", data, 12)
    elf_base = 32 + count * 32
    if data[elf_base:elf_base + 4] != b"\x7fELF":
        raise SystemExit("no embedded ELF header")
    ph_offset = struct.unpack_from("<Q", data, elf_base + 32)[0]
    ph_count = struct.unpack_from("<H", data, elf_base + 56)[0]
    elf_header_size = ph_offset + ph_count * 56
    programs = []
    out_size = elf_header_size
    for i in range(ph_count):
        p_type, _, p_offset, _, _, p_filesz = struct.unpack_from("<IIQQQQ", data, elf_base + ph_offset + i * 56)
        programs.append((p_type, p_offset, p_filesz))
        out_size = max(out_size, p_offset + p_filesz)
    out = bytearray(out_size)
    out[:elf_header_size] = data[elf_base:elf_base + elf_header_size]
    covered = []
    for i in range(count):
        props, offset, stored, decoded = struct.unpack_from("<QQQQ", data, 32 + i * 32)
        if props & 2:
            raise SystemExit("encrypted SELF segment")
        if props & 8:
            raise SystemExit("compressed SELF segment")
        if not props & 0x800:  # digest entry, not data
            continue
        index = (props >> 20) & 0xFFFF
        _, p_offset, p_filesz = programs[index]
        out[p_offset:p_offset + stored] = data[offset:offset + stored]
        covered.append((p_offset, p_offset + stored))
    for i, (p_type, p_offset, p_filesz) in enumerate(programs):
        if p_type == 0x6FFFFF01 and p_filesz:
            out[p_offset:p_offset + p_filesz] = data[file_size:file_size + p_filesz]
            covered.append((p_offset, p_offset + p_filesz))
    retained = elf_header_size
    for i, (p_type, p_offset, p_filesz) in enumerate(programs):
        if not p_filesz:
            continue
        if not any(b <= p_offset and p_offset + p_filesz <= e for b, e in covered):
            if p_type != 4:
                raise SystemExit(f"missing payload for program {i}")
            base = ph_offset + i * 56
            out[base:base + 56] = bytes(56)
            continue
        retained = max(retained, p_offset + p_filesz)
    del out[retained:]
    struct.pack_into("<Q", out, 40, 0)  # e_shoff
    struct.pack_into("<HHH", out, 58, 0, 0, 0)  # e_shentsize, e_shnum, e_shstrndx
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    with open(sys.argv[1], "rb") as f:
        raw = f.read()
    with open(sys.argv[2], "wb") as f:
        f.write(unself(raw))
