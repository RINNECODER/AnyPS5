#!/usr/bin/env python3
"""Bounded, read-only SCE metadata reader; never loads or executes guest code.

SPDX-License-Identifier: GPL-2.0-only
Public metadata adaptation modified 2026-10-07.
Derived from AnyPS5 core/cpu/src/{Self,SceElf}.cpp at
042b41f4c9a03f65c853872f7f9dd189736225f2 (see inspect_sce_metadata.md).
CPU02's retained reader informed field compatibility only; no private data is
included. This reader does not authenticate SELF digests or establish an ABI.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile

LIMIT = 256 * 1024 * 1024
U64 = (1 << 64) - 1
SOURCE_COMMIT = '042b41f4c9a03f65c853872f7f9dd189736225f2'
ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'
SELF_MAGICS = (bytes.fromhex('4f153d1d'), bytes.fromhex('5414f5ee'))
LOAD_TYPES = (1, 0x61000010)
IDENTITY_TAGS = {
    'import_libraries': (0x61000015, 0x61000049),
    'import_modules': (0x6100000f, 0x61000045),
    'export_libraries': (0x61000013, 0x61000047),
    'export_modules': (0x6100000d, 0x61000043),
}
REPEATED_TAGS = {1, 0x61000019, 0x61000017, 0x61000009, 0x61000041}
REPEATED_TAGS.update(tag for tags in IDENTITY_TAGS.values() for tag in tags)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def fits(offset, size, end):
    return 0 <= offset <= end and 0 <= size <= end - offset


def unpack(data, offset, fmt):
    require(fits(offset, struct.calcsize(fmt), len(data)), 'truncated metadata')
    return struct.unpack_from(fmt, data, offset)


def read(data, offset, size):
    require(fits(offset, size, len(data)), 'truncated metadata')
    return int.from_bytes(data[offset:offset + size], 'little')


def elf_headers(data, limit=None):
    end = len(data) if limit is None else limit
    require(fits(0, 64, end) and data[:7] == b'\x7fELF\x02\x01\x01',
            'requires little-endian ELF64 version 1')
    require(read(data, 18, 2) == 62 and read(data, 20, 4) == 1 and read(data, 52, 2) == 64,
            'requires valid x86-64 ELF header')
    phoff, phsize, phcount = read(data, 32, 8), read(data, 54, 2), read(data, 56, 2)
    require(phoff >= 64 and phsize == 56 and 0 < phcount <= 1024
            and fits(phoff, phcount * 56, end), 'invalid program header table')
    programs = [unpack(data, phoff + index * 56, '<IIQQQQQQ') for index in range(phcount)]
    for p in programs:
        require(fits(p[2], p[5], LIMIT), 'program file range overflows or exceeds size limit')
        require(fits(p[3], p[6], U64) and fits(p[4], p[6], U64), 'program address range overflows uint64')
        require(not p[1] & ~7, 'unsupported program permissions')
        if p[0] in LOAD_TYPES:
            require(p[5] <= p[6], 'loadable file size exceeds memory size')
    return phoff, programs


def decode_self(source):
    require(32 <= len(source) <= LIMIT, 'SELF size exceeds supported bounds')
    for offset, size, expected in ((4, 1, 0), (5, 1, 1), (6, 1, 1), (7, 1, 0x12),
                                   (8, 4, 0x101), (26, 2, 0x22), (28, 4, 0)):
        require(read(source, offset, size) == expected, 'unsupported SELF header profile')
    header, meta = read(source, 12, 2), read(source, 14, 2)
    declared, count = read(source, 16, 8), read(source, 24, 2)
    require(0 < count <= 2048 and fits(32, count * 32, header)
            and fits(header, meta, declared) and declared <= len(source), 'invalid SELF header bounds')
    elfbase = 32 + count * 32
    require(fits(elfbase, 64, header), 'truncated embedded ELF header')
    embedded = source[elfbase:header]
    phoff, programs = elf_headers(embedded)
    header_end = phoff + len(programs) * 56
    output_size = max([header_end] + [p[2] + p[5] for p in programs])
    entries = [unpack(source, 32 + index * 32, '<QQQQ') for index in range(count)]
    known = 1 | 2 | 4 | 8 | 0x800 | 0xf000 | 0x10000 | (0xffff << 20)
    data_programs, referenced = set(), set()
    for index, (props, offset, stored, decoded) in enumerate(entries):
        require(not props & 2, 'encrypted SELF segments unsupported')
        require(not props & 8, 'compressed SELF segments unsupported')
        data_entry = bool(props & 0x800)
        require(not props & ~known and (not props & 0x10000 if data_entry else bool(props & 0x10000))
                and (data_entry or not props & 0xf000), 'unsupported SELF segment properties')
        require(stored > 0 and stored == decoded and offset % 16 == 0
                and offset >= header + meta and fits(offset, stored, declared), 'invalid SELF segment range or size')
        for previous in entries[:index]:
            require(offset >= previous[1] + previous[2] or previous[1] >= offset + stored,
                    'overlapping SELF container segments')
        target = (props >> 20) & 0xffff
        if data_entry:
            require(target < len(programs) and target not in data_programs, 'invalid or duplicate SELF data program index')
            data_programs.add(target)
            p = programs[target]
            require(p[0] in (1, 0x61000010, 0x61000000, 0x6fffff00, 0x61000002)
                    and decoded == p[5], 'unsupported SELF data program or size')
        else:
            require(target < len(entries) and entries[target][0] & 0x800 and target not in referenced,
                    'invalid or duplicate SELF digest reference')
            referenced.add(target)
            block = 1 << (12 + ((entries[target][0] >> 12) & 15))
            require(decoded == ((entries[target][3] + block - 1) // block) * 32, 'invalid SELF digest metadata size')
    require(all(i in referenced for i, entry in enumerate(entries) if entry[0] & 0x800),
            'SELF data segment lacks paired digest metadata')
    output, written = bytearray(output_size), []

    def install(offset, payload):
        require(fits(offset, len(payload), len(output)), 'reconstruction range exceeds logical ELF')
        for begin, end in written:
            left, right = max(begin, offset), min(end, offset + len(payload))
            require(left >= right or output[left:right] == payload[left - offset:right - offset],
                    'conflicting reconstructed ELF bytes')
        output[offset:offset + len(payload)] = payload
        written.append((offset, offset + len(payload)))

    install(0, embedded[:header_end])
    for props, offset, stored, _ in entries:
        if props & 0x800:
            install(programs[(props >> 20) & 0xffff][2], source[offset:offset + stored])
    appended = False
    for p in programs:
        if p[0] == 0x6fffff01 and p[5]:
            require(not appended and p[5] == len(source) - declared, 'invalid or unavailable appended SELF version data')
            install(p[2], source[declared:])
            appended = True
    require(len(source) == declared or appended, 'unaccounted trailing SELF bytes')
    written.sort()

    def covered(offset, size):
        cursor = offset
        for left, right in written:
            if left > cursor:
                break
            cursor = max(cursor, right)
            if cursor >= offset + size:
                return True
        return size == 0

    notes, retained_size = [], header_end
    for index, p in enumerate(programs):
        if p[5] and not covered(p[2], p[5]):
            require(p[0] == 4, 'missing required ELF program payload')
            output[phoff + index * 56:phoff + (index + 1) * 56] = bytes(56)
            notes.append('unavailable nonloadable PT_NOTE omitted at program index ' + str(index))
        elif p[5]:
            retained_size = max(retained_size, p[2] + p[5])
    del output[retained_size:]
    if read(output, 40, 8) or any(read(output, offset, 2) for offset in (58, 60, 62)):
        output[40:48] = bytes(8)
        output[58:64] = bytes(6)
        notes.append('SELF section table references removed; reconstruction contains program headers and payloads')
    return output, declared, notes


def identifier(encoded):
    require(1 <= len(encoded) <= 3 and all(c in ALPHABET for c in encoded), 'invalid scoped symbol identifier')
    value = 0
    for character in encoded:
        value = value * 64 + ALPHABET.index(character)
    require(value <= 65535, 'overflowing scoped symbol identifier')
    return value


def bare_name(name):
    return name not in ('', '.', '..') and not any(c in name for c in '/\\:#\r\n')


def parse_metadata(source, path):
    require(64 <= len(source) <= LIMIT, 'input size exceeds supported bounds')
    is_self = source[:4] in SELF_MAGICS
    data, declared, notes = decode_self(source) if is_self else (source, None, [])
    _, programs = elf_headers(data)
    require(read(data, 16, 2) in (3, 0xfe10, 0xfe18), 'unsupported SCE executable type')
    require(read(data, 7, 1) in (0, 9) and read(data, 8, 1) <= 2, 'unsupported SCE OS ABI or ABI version')
    for p in programs:
        require(fits(p[2], p[5], len(data)), 'program segment exceeds input file')
    dynamic = [p for p in programs if p[0] == 2]
    dynlib = [p for p in programs if p[0] == 0x61000000]
    require(len(dynamic) == 1 and 0 < dynamic[0][5] <= 16 * 1024 * 1024 and dynamic[0][5] % 16 == 0,
            'invalid or duplicate dynamic segment')
    require(len(dynlib) <= 1, 'duplicate SCE dynamic data segment')
    tags, repeated, raw_tags = {}, {tag: [] for tag in REPEATED_TAGS}, []
    terminated = False
    for offset in range(dynamic[0][2], dynamic[0][2] + dynamic[0][5], 16):
        tag, value = unpack(data, offset, '<QQ')
        raw_tags.append({'tag': tag, 'value': value})
        if tag == 0:
            terminated = True
            break
        if tag in repeated:
            repeated[tag].append(value)
        elif tag != 0x61000007:
            require(tag not in tags, 'duplicate dynamic tag')
            tags[tag] = value
    require(terminated, 'unterminated dynamic segment')

    def translate(address, length):
        require(fits(address, length, U64), 'dynamic virtual range overflows uint64')
        candidates = [p[2] + address - p[3] for p in programs if p[0] in LOAD_TYPES
                      and address >= p[3] and fits(address - p[3], length, p[5])]
        require(len(candidates) == 1, 'dynamic virtual address has missing or ambiguous file backing')
        return candidates[0]

    def get(standard, sce):
        require((standard in tags) != (sce in tags), 'missing or ambiguous standard/SCE dynamic tag')
        return tags[standard if standard in tags else sce]

    def table(standard, sce, length):
        value = get(standard, sce)
        if standard in tags:
            return translate(value, length)
        require(len(dynlib) == 1 and fits(value, length, dynlib[0][5]), 'SCE table exceeds DYNLIBDATA segment')
        return dynlib[0][2] + value

    strsize = get(10, 0x61000037)
    require(0 < strsize <= LIMIT, 'invalid dynamic string table size')
    stroff = table(5, 0x61000035, strsize)
    strings = bytes(data[stroff:stroff + strsize])
    require(strings[0] == 0, 'invalid dynamic string table null entry')

    def string(offset):
        require(offset < len(strings), 'dynamic string offset exceeds table')
        end = strings.find(b'\0', offset)
        require(end >= 0, 'unterminated dynamic string')
        try:
            value = strings[offset:end].decode('ascii')
        except UnicodeDecodeError as error:
            raise ValueError('non-ASCII SCE metadata string unsupported') from error
        require(all(ord(c) >= 32 and ord(c) != 127 for c in value), 'control character in metadata string')
        return value

    identities = {}
    for key, keys in IDENTITY_TAGS.items():
        result = {}
        for tag in keys:
            for value in repeated[tag]:
                index = value >> 48
                identity = {'id': index, 'name': string(value & 0xffffffff), 'version': (value >> 32) & 0xffff}
                require(bare_name(identity['name']), 'invalid module or library name')
                require(index not in result or result[index] == identity, 'conflicting module or library identity')
                result[index] = identity
        identities[key] = result

    attributes = {}
    for direction, tag in (('import', 0x61000019), ('export', 0x61000017)):
        result = {}
        for value in repeated[tag]:
            index, attribute = value >> 48, value & 0xffffffffffff
            require(index in identities[direction + '_libraries'], 'attribute has no declared library identity')
            require(index not in result or result[index] == attribute, 'conflicting library attributes')
            result[index] = attribute
        attributes[direction + '_attributes'] = [{'library_id': index, 'attributes': value}
                                                 for index, value in sorted(result.items())]
    original = [string(value) for tag in (0x61000009, 0x61000041) for value in repeated[tag]]
    require(len(set(original)) <= 1, 'conflicting original filename metadata')
    require(all(bare_name(name) for name in original), 'invalid original filename')
    needed = [string(value) for value in repeated[1]]
    require(all(bare_name(name) for name in needed), 'invalid DT_NEEDED filename')
    require(get(11, 0x6100003b) == 24, 'unsupported dynamic symbol entry size')
    if 0x6100003f in tags:
        symsize = tags[0x6100003f]
    else:
        require(4 in tags, 'missing dynamic symbol count (GNU hash alone unsupported)')
        hash_offset = translate(tags[4], 8)
        buckets, chains = unpack(data, hash_offset, '<II')
        require(buckets > 0 and chains > 0 and buckets <= 1024 * 1024 and chains <= 1024 * 1024,
                'invalid SysV hash table counts')
        translate(tags[4], 8 + (buckets + chains) * 4)
        symsize = chains * 24
    require(0 < symsize <= 24 * 1024 * 1024 and symsize % 24 == 0, 'invalid dynamic symbol table size')
    if 4 in tags and 0x6100003f in tags:
        hash_offset = translate(tags[4], 8)
        buckets, chains = unpack(data, hash_offset, '<II')
        require(0 < buckets <= 1024 * 1024 and chains == symsize // 24, 'conflicting dynamic symbol counts')
        translate(tags[4], 8 + (buckets + chains) * 4)
    symoff = table(6, 0x61000039, symsize)
    imports, exports, symbols = [], [], []
    for index, offset in enumerate(range(symoff, symoff + symsize, 24)):
        nameoff, info, visibility, section, value, size = unpack(data, offset, '<IBBHQQ')
        name, binding, kind = string(nameoff), info >> 4, info & 15
        require(binding <= 2 and visibility <= 3, 'unsupported dynamic symbol binding or visibility')
        require(fits(value, size, U64), 'symbol value/size overflows uint64')
        require(index != 0 or not any((nameoff, info, visibility, section, value, size)), 'invalid null dynamic symbol')
        item = {'name': name, 'symbol_index': index, 'type': kind, 'binding': binding,
                'visibility': visibility, 'section': section, 'value': value, 'size': size}
        direction = 'import' if index and not section and binding else 'export' if section and binding and '#' in name else None
        if direction:
            parts = name.split('#')
            require(len(parts) == 3 and len(parts[0]) == 11 and all(c in ALPHABET for c in parts[0]),
                    'symbol requires full NID#library#module scope')
            lid, mid = identifier(parts[1]), identifier(parts[2])
            libs, mods = identities[direction + '_libraries'], identities[direction + '_modules']
            require(lid in libs and mid in mods, 'symbol qualifier has no declared library or module identity')
            library, module = libs[lid], mods[mid]
            item.update({'nid': parts[0], 'scoped_name': name, 'library': library['name'], 'library_id': lid,
                         'library_version': library['version'], 'module': module['name'], 'module_id': mid,
                         'module_major': module['version'] >> 8, 'module_minor': module['version'] & 255})
            scoped = {key: value for key, value in item.items() if key != 'name'}
            (imports if direction == 'import' else exports).append(scoped)
        symbols.append(item)
    result = {'schema_version': 1, 'path': str(path), 'size': len(source),
              'sha256': hashlib.sha256(source).hexdigest(), 'container': 'plain_self' if is_self else 'elf',
              'elf_type': read(data, 16, 2), 'elf_os_abi': read(data, 7, 1), 'elf_abi_version': read(data, 8, 1),
              'entry': read(data, 24, 8), 'needed_files': sorted(needed), 'imports': imports, 'exports': exports,
              'symbols': symbols, 'original_filenames': sorted(set(original)), 'normalization_notes': notes,
              'dynamic_tags': raw_tags, 'module_attributes': [{'tag': 0x61000011, 'value': tags[0x61000011],
                  'qualification': 'unknown', 'reason': 'module attribute scope/semantics not established by source'}]
                  if 0x61000011 in tags else [],
              'producer': {'source_commit': SOURCE_COMMIT, 'source_paths': ['core/cpu/src/Self.cpp', 'core/cpu/src/SceElf.cpp'],
                           'tool_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()},
              'unknowns': ['Static metadata does not prove mapped/readable object storage, TLS backing, executable storage, '
                           'SELF authentication, relocation validity, execution profile, service ABI or runtime behavior.']}
    for key, values in identities.items():
        result[key] = [values[index] for index in sorted(values)]
    result.update(attributes)
    if declared is not None:
        result['declared_self_size'] = declared
    return result


def inspect(path):
    path = Path(path)
    require(not path.is_symlink(), 'input symlink unsupported')
    path = path.resolve(strict=True)
    descriptor = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    try:
        before = os.fstat(descriptor)
        require(stat.S_ISREG(before.st_mode) and 64 <= before.st_size <= LIMIT, 'input must be a bounded regular file')
        with os.fdopen(descriptor, 'rb', closefd=False) as handle:
            source = handle.read(LIMIT + 1)
        after = os.fstat(descriptor)
        require(len(source) == before.st_size and (before.st_size, before.st_mtime_ns, before.st_ctime_ns)
                == (after.st_size, after.st_mtime_ns, after.st_ctime_ns), 'input changed during read')
    finally:
        os.close(descriptor)
    return parse_metadata(source, path)


def publish(output, payload):
    output = Path(output)
    require(not os.path.lexists(output), 'output already exists (including symlinks)')
    parent = output.parent.resolve(strict=True)
    require(parent.is_dir(), 'output parent must be an existing directory')
    require(not any((ancestor / '.git').exists() for ancestor in (parent, *parent.parents)),
            'metadata output must be outside Git repositories')
    require(output.name not in ('', '.', '..'), 'invalid output filename')
    output = parent / output.name
    descriptor, temporary = tempfile.mkstemp(prefix='.sce-metadata-', dir=parent)
    try:
        with os.fdopen(descriptor, 'wb') as handle:
            os.fchmod(handle.fileno(), 0o600)
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.link(temporary, output, follow_symlinks=False)
    finally:
        os.unlink(temporary)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--input', required=True, help='explicit plaintext ELF/SCE/SELF regular file')
    parser.add_argument('--output', required=True, help='fresh JSON path outside Git; parent must exist')
    arguments = parser.parse_args()
    try:
        require(not os.path.lexists(arguments.output), 'output already exists (input overwrite forbidden)')
        result = inspect(arguments.input)
        payload = (json.dumps(result, indent=2, sort_keys=True, ensure_ascii=True) + '\n').encode('utf-8')
        output = publish(arguments.output, payload)
        print(json.dumps({'output': str(output), 'sha256': hashlib.sha256(payload).hexdigest(),
                          'imports': len(result['imports']), 'exports': len(result['exports'])}, sort_keys=True))
        return 0
    except (ValueError, OSError) as error:
        # Do not echo source names, symbol scopes, or payload bytes on failure.
        message = str(error) if isinstance(error, ValueError) else error.strerror
        print('SCE metadata reader: ' + (message or 'filesystem operation failed'), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
