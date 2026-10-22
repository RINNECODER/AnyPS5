#!/usr/bin/env python3
"""Offline registry/name-export audit, not a runtime provider or ABI qualification.

Adapted from abderahmane-ai, upstream PR1461 (9e6a6004a5e8a251af8e19eab66b1ac3cad71bab),
archived main aa277b3818d8cf7bc900bdfee740f318469df349, with PE ordinal/address
correction by yahya308, PR1800 (fd50daeb240ddb766690de6a077bb0a383c776a3).
GNU GPL version 2; see repository LICENSE.
"""
import argparse
import json
import re
import struct
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

import nid_names
import progress

ROOT = Path(__file__).resolve().parent.parent
PRX = ROOT / 'core' / 'libs' / 'prx'
NID = re.compile(r'[A-Za-z0-9+\-]{11}')
EXPORT_ALIAS = re.compile(r'APS5_EXPORT\("([A-Za-z0-9+\-]{11})",\s*(\w+)\)')
NID_POSTFIX = '_nid_postfix'
CLASSES = ('export-present', 'export-missing', 'export-unknown')
PE_DIRECTORY_OFFSET = {0x20b: 112, 0x10b: 96}


class AuditError(Exception):
    pass


def take(data, offset, size, path, what):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise AuditError(f'{path}: {what} at 0x{offset:x} with size {size} lies outside the file ({len(data)} bytes)')
    return data[offset:offset + size]


def unpack(layout, data, offset, path, what):
    return struct.unpack(layout, take(data, offset, struct.calcsize(layout), path, what))


def cstring(data, offset, limit, path, what):
    end = data.find(b'\0', offset, limit)
    if offset < 0 or offset >= limit or end < 0:
        raise AuditError(f'{path}: {what} at 0x{offset:x} is not a NUL-terminated string inside 0x{limit:x} bytes')
    try:
        return data[offset:end].decode('utf-8')
    except UnicodeDecodeError as error:
        raise AuditError(f'{path}: {what} at 0x{offset:x} is not UTF-8: {error}') from error


def elf_exports(path, data):
    header = unpack('<16sHHIQQQIHHHHHH', data, 0, path, 'ELF header')
    ident, section_offset, section_size, section_count = header[0], header[6], header[11], header[12]
    if ident[4:7] != b'\x02\x01\x01' or header[3] != 1 or section_size != 64:
        raise AuditError(f'{path}: only little-endian ELF64 with 64-byte section headers is supported')
    if not section_count:
        raise AuditError(f'{path}: missing or extended ELF section inventory is unsupported')
    take(data, section_offset, section_count * 64, path, 'section headers')
    sections = [unpack('<IIQQQQIIQQ', data, section_offset + i * 64, path, f'section header {i}') for i in range(section_count)]
    names = set()
    for section in sections:
        kind, offset, size, link, entry = section[1], section[4], section[5], section[6], section[9]
        if kind != 11:
            continue
        if entry != 24 or size % 24 or link >= len(sections) or sections[link][1] != 3:
            raise AuditError(f'{path}: invalid dynamic symbol table (entry size {entry}, size {size}, string table section {link})')
        take(data, offset, size, path, 'dynamic symbol table')
        strings_offset, strings_size = sections[link][4], sections[link][5]
        take(data, strings_offset, strings_size, path, 'dynamic string table')
        for position in range(0, size, 24):
            name, info, other, index, value, _ = unpack('<IBBHQQ', data, offset + position, path, 'dynamic symbol')
            if name >= strings_size:
                raise AuditError(f'{path}: dynamic symbol name offset {name} outside string table')
            symbol_name = cstring(data, strings_offset + name, strings_offset + strings_size, path, 'export name')
            if index == 0xffff or (index >= len(sections) and index < 0xff00):
                raise AuditError(f'{path}: invalid or unsupported dynamic symbol section {index}')
            binding, visibility = info >> 4, other & 3
            if index and value and binding in (1, 2, 10) and visibility in (0, 3) and symbol_name:
                names.add(symbol_name)
    return names


def pe_exports(path, data):
    header = unpack('<I', data, 0x3c, path, 'PE header offset')[0]
    if unpack('<I', data, header, path, 'PE signature')[0] != 0x4550:
        raise AuditError(f'{path}: invalid PE signature at 0x{header:x}')
    section_count = unpack('<H', data, header + 6, path, 'section count')[0]
    optional_size = unpack('<H', data, header + 20, path, 'optional header size')[0]
    optional = header + 24
    magic = unpack('<H', data, optional, path, 'optional header magic')[0]
    if magic not in PE_DIRECTORY_OFFSET:
        raise AuditError(f'{path}: unsupported PE optional header magic 0x{magic:x}')
    directory = PE_DIRECTORY_OFFSET[magic]
    if optional_size < directory + 8:
        raise AuditError(f'{path}: PE optional header of {optional_size} bytes has no export directory')
    take(data, optional, optional_size, path, 'PE optional header')
    if not unpack('<I', data, optional + directory - 4, path, 'data directory count')[0]:
        raise AuditError(f'{path}: PE optional header declares no export directory')
    rva, size = unpack('<II', data, optional + directory, path, 'export data directory')
    sections = [unpack('<8sIIIIIIHHI', data, optional + optional_size + i * 40, path, f'PE section {i}') for i in range(section_count)]

    def to_offset(address, length):
        for section in sections:
            virtual, raw_size, raw_offset = section[2], section[3], section[4]
            relative = address - virtual
            if relative < 0 or relative >= raw_size or length > raw_size - relative:
                continue
            take(data, raw_offset, raw_size, path, f'section at RVA 0x{virtual:x}')
            return raw_offset + relative, raw_offset + raw_size
        raise AuditError(f'{path}: RVA 0x{address:x} is not inside any section')

    if rva == size == 0:
        return set()
    if not rva or size < 40:
        raise AuditError(f'{path}: invalid PE export directory')
    table_offset, _ = to_offset(rva, size)
    table = unpack('<IIHHIIIIIII', data, table_offset, path, 'export directory')
    address_count, name_count, addresses_rva, names_rva, ordinals_rva = table[6:11]
    addresses_offset = to_offset(addresses_rva, address_count * 4)[0] if address_count else None
    # Validate all nonzero slots, even ordinal-only exports, without resolving forwarders.
    for i in range(address_count):
        address = unpack('<I', data, addresses_offset + i * 4, path, 'export address')[0]
        if address:
            offset, limit = to_offset(address, 1)
            if rva <= address < rva + size:
                cstring(data, offset, min(limit, offset + rva + size - address), path, 'forwarder name')
    names = set()
    if not name_count:
        return names
    names_offset, _ = to_offset(names_rva, name_count * 4)
    ordinals_offset, _ = to_offset(ordinals_rva, name_count * 2)
    for i in range(name_count):
        ordinal = unpack('<H', data, ordinals_offset + i * 2, path, 'export ordinal')[0]
        if ordinal >= address_count:
            raise AuditError(f'{path}: export ordinal {ordinal} for name {i} is outside the address table ({address_count} entries)')
        name_rva = unpack('<I', data, names_offset + i * 4, path, 'export name RVA')[0]
        offset, limit = to_offset(name_rva, 1)
        name = cstring(data, offset, limit, path, 'export name')
        address = unpack('<I', data, addresses_offset + ordinal * 4, path, 'export address')[0]
        if address and name:
            names.add(name)
    return names


def read_exports(path):
    data = path.read_bytes()
    if data[:4] == b'\x7fELF':
        return elf_exports(path, data)
    if data[:2] == b'MZ':
        return pe_exports(path, data)
    raise AuditError(f'{path}: unsupported format; only ELF/PE are supported (Mach-O is not supported; first bytes {data[:4]!r})')


def directory_files(directory, pattern):
    path = Path(directory)
    if not path.is_dir():
        raise AuditError(f'{directory}: not a directory')
    return sorted(item for item in path.glob(pattern) if item.is_file())


def built_libraries(directories):
    exports = defaultdict(set)
    libraries = set()
    for directory in directories:
        files = directory_files(directory, '*.prx')
        if not files:
            raise AuditError(f'{directory}: no .prx files; pass the patched libs output directory')
        for file in files:
            libraries.add(file.name)
            for name in read_exports(file):
                exports[name].add(file.name)
    return {name: sorted(files) for name, files in exports.items()}, libraries


def module_files(directories):
    return {file.name for directory in directories for file in directory_files(directory, '*')}


def nid_of(name, aliases):
    return aliases[name] if name in aliases else nid_names.compute_nid(name.removesuffix(NID_POSTFIX))


def source_stubs(source):
    if not Path(source).is_dir():
        raise AuditError(f'{source}: not a directory')
    pending = {}
    for library in sorted(item for item in Path(source).iterdir() if item.is_dir()):
        aliases = {}
        for file in library.rglob('*.cpp'):
            for nid, name in EXPORT_ALIAS.findall(file.read_text(errors='ignore')):
                aliases[name] = nid
        group = progress.scan_library(library)
        for name in group['todo_names']:
            pending.setdefault(nid_of(name, aliases), name)
    # These are source hints only: another non-throwing definition is not ABI proof.
    return pending


def read_registry(path):
    try:
        entries = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AuditError(f'{path}: cannot read registry: {error}') from error
    if not isinstance(entries, list):
        raise AuditError(f'{path}: a registry is a JSON list, got {type(entries).__name__}')
    imports = []
    for i, entry in enumerate(entries):
        if not isinstance(entry, dict) or not isinstance(entry.get('nid'), str) or not isinstance(entry.get('library'), str):
            raise AuditError(f"{path}: entry {i} needs string fields 'nid' and 'library': {entry!r}")
        nid = entry['nid'].split('#', 1)[0]
        if not NID.fullmatch(nid):
            raise AuditError(f"{path}: entry {i} has an invalid NID {entry['nid']!r}")
        imports.append((nid, entry['library']))
    return imports


def check_conservation(references, records):
    classified = sum(record['references'] for record in records)
    unknown = sorted({record['class'] for record in records} - set(CLASSES))
    if classified != references or unknown:
        raise AuditError(f'conservation failed: {references} references in, {classified} classified, unknown classes {unknown}')


def audit(imports, exports, stubs, libraries, modules):
    records = {}
    for nid, library in imports:
        record = records.setdefault((nid, library), {'nid': nid, 'library': library, 'references': 0})
        record['references'] += 1
    for record in records.values():
        nid, library = record['nid'], record['library']
        providers = exports.get(nid, [])
        record['class'] = 'export-present' if providers else 'export-unknown' if library in modules else 'export-missing'
        record['providers'] = providers
        record['library_mismatch'] = bool(providers and library and library not in providers)
        record['module_filename_present'] = library in modules
        record['source_hint'] = stubs.get(nid)
        record['name'] = stubs.get(nid)
    ordered = sorted(records.values(), key=lambda record: (record['class'], record['library'], record['nid']))
    check_conservation(len(imports), ordered)
    missing = defaultdict(int)
    for record in ordered:
        library = record['library']
        if library and library not in libraries and library not in modules:
            missing[library] += record['references']
    return ordered, dict(sorted(missing.items()))


def load_names(path):
    path = Path(path)
    if not path.is_file():
        raise AuditError(f'{path}: names file not found; the audit never downloads one')
    # Read locally rather than using load_db's downloading fallback after a race.
    names = {}
    for line in path.read_text(errors='replace').splitlines():
        parts = line.strip().split()
        if len(parts) >= 2:
            names.setdefault(parts[0], parts[1])
    return names


def attach_names(records, names):
    for record in records:
        if record['name'] is None and record['nid'] in names:
            name = names[record['nid']]
            record['name'] = name
            record['name_verified'] = nid_names.compute_nid(name) == record['nid']


def git_stamp():
    try:
        commit = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True, timeout=5).stdout.strip()
        dirty = bool(subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain'], capture_output=True, text=True, check=True, timeout=5).stdout.strip())
    except (OSError, subprocess.SubprocessError):
        return None, None
    return commit, dirty


def summarize(records, references, missing):
    return {'references': references, 'unique_imports': len(records),
        'unique_by_class': {kind: sum(r['class'] == kind for r in records) for kind in CLASSES},
        'references_by_class': {kind: sum(r['references'] for r in records if r['class'] == kind) for kind in CLASSES},
        'missing_libraries': missing, 'library_mismatch': sum(r['library_mismatch'] for r in records)}


def render(summary, records, missing):
    lines = ['Offline name-export evidence only; runtime ABI/callability NOT_ESTABLISHED.',
             'Module/version identity unknown; no firmware, loader binding or game-support claim.',
             f"{summary['references']} references, {summary['unique_imports']} unique imports"]
    for kind in CLASSES:
        lines.append(f"  {kind:<16}{summary['unique_by_class'][kind]:>6} imports  {summary['references_by_class'][kind]:>6} references")
    for record in records:
        hint = f"; source throwing-stub hint: {record['source_hint']} (not ABI proof)" if record['source_hint'] else ''
        name = f" {record['name']}" if record['name'] else ''
        if record.get('name_verified') is False:
            name += ' (name does not hash to this NID)'
        lines.append(f"  [{record['library'] or 'library not named'}] {record['nid']}{name}: {record['class']}{hint}")
    if missing:
        lines.append(f'Missing library filenames: {missing}')
    if summary['library_mismatch']:
        lines.append(f"{summary['library_mismatch']} imports have name exports only in other libraries; binding NOT_ESTABLISHED.")
    return '\n'.join(lines)


def check_output(output, args):
    if output.exists() or output.is_symlink():
        raise AuditError(f'{output}: output already exists; refusing to overwrite')
    resolved = output.resolve()
    inputs = [*args.registries, *([Path(args.names)] if args.names else [])]
    if any(resolved == p.resolve() for p in inputs):
        raise AuditError(f'{output}: output aliases an input')
    for directory in [*args.libs, *args.modules, args.source]:
        if resolved.is_relative_to(Path(directory).resolve()):
            raise AuditError(f'{output}: output must be outside input source/library/module trees')


def main(argv=None):
    parser = argparse.ArgumentParser(description='offline relinker registry audit against patched ELF/PE name exports; runtime ABI/callability NOT_ESTABLISHED')
    parser.add_argument('registries', nargs='+', type=Path, help="JSON written by 'relinker --registry'")
    parser.add_argument('--libs', action='append', required=True, help='directory of patched ELF/PE .prx files; repeat for several')
    parser.add_argument('--modules', action='append', default=[], help='module filenames only; their exports remain unknown')
    parser.add_argument('--source', default=str(PRX), help='source tree for throwing-stub hints only')
    parser.add_argument('--names', help='local NID/name file; never downloaded')
    parser.add_argument('--json', type=Path, help='new report file outside input trees; never overwritten')
    args = parser.parse_args(argv)
    try:
        if args.json:
            check_output(args.json, args)
        imports = [entry for registry in args.registries for entry in read_registry(registry)]
        exports, libraries = built_libraries(args.libs)
        modules = module_files(args.modules)
        records, missing = audit(imports, exports, source_stubs(args.source), libraries, modules)
        if args.names:
            attach_names(records, load_names(args.names))
        summary = summarize(records, len(imports), missing)
        if args.json:
            commit, dirty = git_stamp()
            result = {'audit_tool_commit': commit, 'audit_tool_dirty': dirty,
                'source_hint_root': str(Path(args.source).resolve()), 'registries': [str(p) for p in args.registries],
                'runtime_abi_callability': 'NOT_ESTABLISHED', 'scope_identity': 'module/version identity unknown',
                **summary, 'imports': records}
            # Exclusive creation also refuses an alias introduced after validation.
            with args.json.open('x', encoding='utf-8') as output:
                output.write(json.dumps(result, indent=2) + '\n')
    except (AuditError, OSError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 2
    print(render(summary, records, missing))
    return 1 if summary['unique_by_class']['export-missing'] or summary['unique_by_class']['export-unknown'] or missing or summary['library_mismatch'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
