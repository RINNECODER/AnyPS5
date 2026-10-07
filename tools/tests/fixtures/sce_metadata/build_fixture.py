"""Compile public assembly and add independently specified SCE metadata.

No production parser imports. ELF64 layout follows System V generic ABI:
https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.symtab.html
SCE tag values and packed IDs corroborate shadPS4's public elf.h and AnyPS5
042b41f4:core/cpu/src/SceElf.cpp. SELF wrapper follows that revision's Self.cpp.
Only encoding/layout facts are used; no upstream source text is copied.
Fixture ABI names and expectations are invented public controls, except the
actual CommonDialog NID uoUpLGNkygk from that pinned resolver registration.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

FIXTURES = Path(__file__).resolve().parent
SYMBOLS = {
    'host_function': ('uoUpLGNkygk', 1),
    'guest_function': ('GuestFun001', 2),
    'absent_function': ('AbsentFun01', 2),
    'wrong_type': ('WrongType01', 2),
    'guest_object': ('GuestObj001', 2),
    'guest_tls': ('GuestTls001', 2),
}


def tool(name, override=None):
    candidate = override or shutil.which(name)
    if name == 'clang' and not override:
        homebrew = Path('/opt/homebrew/opt/llvm/bin/clang')
        if homebrew.is_file():
            candidate = str(homebrew)
    if not candidate:
        raise RuntimeError(f'{name} required to compile the public fixture')
    return candidate


def compile_fixture(output, clang=None, lld=None):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    commands = []
    for kind in ('consumer', 'provider'):
        obj, linked = output / (kind + '.o'), output / (kind + '.elf')
        command = [tool('clang', clang), '--target=x86_64-unknown-linux-gnu', '-c',
                   str(FIXTURES / (kind + '.S')), '-o', str(obj)]
        subprocess.run(command, check=True, capture_output=True, timeout=30)
        commands.append(command)
        command = [tool('ld.lld', lld), '-shared', '--hash-style=sysv', '--build-id=none',
                   '-o', str(linked), str(obj)]
        subprocess.run(command, check=True, capture_output=True, timeout=30)
        commands.append(command)
        sce, layout = package(linked.read_bytes(), kind)
        (output / ('main.bin' if kind == 'consumer' else 'fixture.prx')).write_bytes(sce)
        (output / (kind + '.layout.json')).write_text(json.dumps(layout, indent=2) + '\n')
    return commands


def package(raw, kind):
    image = bytearray(raw)
    if raw[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<HH', raw, 16) != (3, 62):
        raise ValueError('requires linker-produced x86-64 ELF64 ET_DYN')
    phoff, = struct.unpack_from('<Q', raw, 32)
    phsize, phcount = struct.unpack_from('<HH', raw, 54)
    shoff, = struct.unpack_from('<Q', raw, 40)
    shsize, shcount = struct.unpack_from('<HH', raw, 58)
    if (phsize, shsize) != (56, 64):
        raise ValueError('unsupported compiler ELF layout')
    headers = [struct.unpack_from('<IIQQQQQQ', raw, phoff + i * 56) for i in range(phcount)]
    sections = [struct.unpack_from('<IIQQQQIIQQ', raw, shoff + i * 64) for i in range(shcount)]
    dynsym = next(row for row in sections if row[1] == 11)
    dynstr = sections[dynsym[6]]
    old_strings = raw[dynstr[4]:dynstr[4] + dynstr[5]]
    symbols = bytearray(raw[dynsym[4]:dynsym[4] + dynsym[5]])
    strings = bytearray(b'\0')

    def string(value):
        offset = len(strings)
        strings.extend(value.encode('ascii') + b'\0')
        return offset

    symbol_indices = {}
    for index, offset in enumerate(range(0, len(symbols), 24)):
        nameoff, info, visibility, section, value, size = struct.unpack_from('<IBBHQQ', symbols, offset)
        name = old_strings[nameoff:old_strings.index(0, nameoff)].decode('ascii')
        if name in SYMBOLS:
            nid, identity = SYMBOLS[name]
            suffix = 'B' if identity == 1 else 'C'
            name = f'{nid}#{suffix}#{suffix}'
            symbol_indices[nid] = index
        if index:
            struct.pack_into('<I', symbols, offset, string(name))
    expected = set(SYMBOLS) if kind == 'consumer' else set(SYMBOLS) - {'host_function', 'absent_function'}
    if {nid for name, (nid, _) in SYMBOLS.items() if name in expected} != set(symbol_indices):
        raise ValueError('linker did not retain complete public ABI control')
    names = {name: string(name) for name in ('libSceCommonDialog', 'fixtureModule', 'fixtureLibrary',
                                          'libSceCommonDialog.prx', 'fixture.prx', 'missing.prx')}
    data = bytearray(strings)
    data.extend(b'\0' * (-len(data) % 8))
    symoff = len(data)
    data.extend(symbols)
    tags = []

    def identity(tag, name, version, identity_id):
        tags.append((tag, (identity_id << 48) | (version << 32) | names[name]))

    if kind == 'consumer':
        identity(0x61000045, 'libSceCommonDialog', 0x101, 1)
        identity(0x61000049, 'libSceCommonDialog', 1, 1)
        identity(0x61000045, 'fixtureModule', 0x101, 2)
        identity(0x61000049, 'fixtureLibrary', 1, 2)
        tags += [(1, names[name]) for name in ('libSceCommonDialog.prx', 'fixture.prx', 'missing.prx')]
        tags += [(0x61000019, (2 << 48) | 0x12345)]
    else:
        identity(0x61000043, 'fixtureModule', 0x101, 2)
        identity(0x61000047, 'fixtureLibrary', 1, 2)
        tags += [(0x61000017, (2 << 48) | 0x23456)]
    tags += [(0x61000011, 0x123456789abc)]
    tags += [(0x61000035, 0), (0x61000037, len(strings)), (0x61000039, symoff),
             (0x6100003b, 24), (0x6100003f, len(symbols)), (0, 0)]
    image.extend(b'\0' * (-len(image) % 8))
    dataoff = len(image)
    image.extend(data)
    image.extend(b'\0' * (-len(image) % 8))
    dynoff = len(image)
    image.extend(b''.join(struct.pack('<QQ', *row) for row in tags))
    kept = [row for row in headers if row[0] in (1, 7, 0x6474e551)]
    kept += [(2, 4, dynoff, 0, 0, len(tags) * 16, len(tags) * 16, 8),
             (0x61000000, 4, dataoff, 0, 0, len(image) - dataoff, 0, 8)]
    image.extend(b'\0' * (-len(image) % 8))
    table = len(image)
    image.extend(b''.join(struct.pack('<IIQQQQQQ', *row) for row in kept))
    image[7:9] = bytes((9, 2))
    struct.pack_into('<H', image, 16, 0xfe10 if kind == 'consumer' else 0xfe18)
    struct.pack_into('<QQ', image, 32, table, 0)
    struct.pack_into('<H', image, 56, len(kept))
    struct.pack_into('<HHH', image, 58, 0, 0, 0)
    return bytes(image), {'dynamic_offset': dynoff, 'data_offset': dataoff,
                          'symbol_offset': dataoff + symoff, 'symbols': symbol_indices,
                          'tag_offsets': {str(tag): dynoff + i * 16 for i, (tag, _) in enumerate(tags)},
                          'program_offset': table, 'program_count': len(kept), 'string_size': len(strings)}


def plain_self(raw):
    """Wrap exact ELF bytes in public plaintext data + paired digest entries."""
    phoff, = struct.unpack_from('<Q', raw, 32)
    count, = struct.unpack_from('<H', raw, 56)
    programs = [struct.unpack_from('<IIQQQQQQ', raw, phoff + i * 56) for i in range(count)]
    segments = [(i, p) for i, p in enumerate(programs) if p[5] and p[0] in (1, 0x61000000)]
    entries_count = 2 * len(segments)
    prefix = raw[:phoff + count * 56]
    header_size = (32 + entries_count * 32 + len(prefix) + 15) & ~15
    result = bytearray(header_size)
    result[0:12] = bytes.fromhex('4f153d1d0001011201010000')
    struct.pack_into('<HH', result, 12, header_size, 0)
    struct.pack_into('<HH', result, 24, entries_count, 0x22)
    result[32 + entries_count * 32:32 + entries_count * 32 + len(prefix)] = prefix
    entries = []
    for program_index, program in segments:
        result.extend(b'\0' * (-len(result) % 16))
        offset = len(result)
        payload = raw[program[2]:program[2] + program[5]]
        result.extend(payload)
        data_index = len(entries)
        entries.append((0x800 | (program_index << 20), offset, len(payload), len(payload)))
        result.extend(b'\0' * (-len(result) % 16))
        digest_offset = len(result)
        digests = b''.join(hashlib.sha256(payload[i:i+4096]).digest() for i in range(0, len(payload), 4096))
        result.extend(digests)
        entries.append((0x10000 | (data_index << 20), digest_offset, len(digests), len(digests)))
    struct.pack_into('<Q', result, 16, len(result))
    for i, entry in enumerate(entries):
        struct.pack_into('<QQQQ', result, 32 + i * 32, *entry)
    return bytes(result)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True)
    parser.add_argument('--clang')
    parser.add_argument('--lld')
    args = parser.parse_args()
    commands = compile_fixture(args.output_dir, args.clang, args.lld)
    print(json.dumps({'commands': commands, 'output_dir': str(Path(args.output_dir).resolve())}))
