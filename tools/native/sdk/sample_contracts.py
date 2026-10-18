#!/usr/bin/env python3
"""Generic: turn any x86-64 ELF into Image-valid (schema 1) contracts for the native AOT pipeline.

This is the non-title counterpart of tools/native/contracts/build_title_contracts.py: no ABI file,
no gap tables, no 0x80 padding entry. Function starts come from recover.recover_image (ELF symbols +
.eh_frame FDEs); indirect-call/jump target sets come from generate_contracts.py. Internal functions
are uniform; imports are stubs unless signatures are supplied; data imports get zero bindings.
"""
import hashlib
import json
import re

from elftools.elf.elffile import ELFFile

import recover
from dynamic import parse_dynamic

FLOATING_POINT = {'rounding': 'nearest-even', 'subnormals': 'preserve', 'exceptions': 'masked',
                  'status_flags': 'unobserved', 'nan_payloads': 'unobserved'}


def _sanitize(name, used):
    base = re.sub(r'[^0-9A-Za-z_]', '_', name or 'import')
    if not base or base[0].isdigit():
        base = '_' + base
    candidate = base
    counter = 0
    while candidate in used:
        counter += 1
        candidate = f'{base}_{counter}'
    used.add(candidate)
    return candidate


def starts(elf_path):
    """address -> size, from symbols and .eh_frame FDEs (recover.recover_image)."""
    recovery = recover.recover_image(elf_path)
    return {item['address']: item['size'] for item in recovery['functions']}


def build_contracts(elf_path, recovery, signatures=None):
    """Assemble the schema-1 contract set the compiler consumes from a generate_contracts recovery."""
    raw = open(elf_path, 'rb').read()
    signatures = signatures or {}
    target_sets = recovery['target_sets']
    addresses = sorted(spec['address'] for spec in recovery['functions'])
    address_index = {address: index for index, address in enumerate(addresses)}
    with open(elf_path, 'rb') as handle:
        elf = ELFFile(handle)
        loads = [segment for segment in elf.iter_segments() if segment['p_type'] == 'PT_LOAD']
        executable = [segment for segment in loads if segment['p_flags'] & 1]
        code_hi = max(segment['p_vaddr'] + segment['p_filesz'] for segment in executable)
        has_tls = any(segment['p_type'] == 'PT_TLS' and segment['p_memsz'] for segment in elf.iter_segments())
        entry = elf['e_entry']

        def read(address, size):
            segment = next(item for item in loads
                           if item['p_vaddr'] <= address and address + size <= item['p_vaddr'] + item['p_filesz'])
            elf.stream.seek(segment['p_offset'] + address - segment['p_vaddr'])
            return elf.stream.read(size)

        dynamic = parse_dynamic(elf, read)

    symbols = dynamic['symbols']
    function_imports, data_imports = {}, {}
    for relocation in dynamic['relocations']:
        symbol = symbols[relocation['symbol_index']]
        name = symbol['name']
        if not name:
            continue
        if relocation['kind'] in (6, 7) and symbol['info'] & 15 != 1:
            function_imports[name] = None
        elif relocation['kind'] in (1, 6, 7):
            data_imports[name] = None

    used = set()
    imports = {}
    for name in function_imports:
        if name in signatures:
            result, parameters = signatures[name]
            imports[name] = {'name': _sanitize(name, used), 'result': result, 'parameters': parameters}
        else:
            imports[name] = {'name': _sanitize(name, used), 'result': 'i64', 'parameters': [], 'stub': True}
    data_bindings = {name: {'name': _sanitize('data_' + name, used), 'size': 4096}
                     for name in data_imports}

    def body_end(address):
        index = address_index[address]
        return addresses[index + 1] if index + 1 < len(addresses) else code_hi

    functions = []
    for address in addresses:
        spec = signatures.get(f'sub_{address:x}') or signatures.get(hex(address))
        result, parameters = spec if spec else ('void', [])
        functions.append({'name': f'sub_{address:x}', 'address': address, 'size': body_end(address) - address,
                          'uniform': True, 'result': result, 'parameters': parameters})

    contracts = {'schema': 1, 'sha256': hashlib.sha256(raw).hexdigest(),
                 'max_segment_bytes': 256 * 1024 * 1024, 'floating_point': FLOATING_POINT,
                 'allow_unlowered_tls': True, 'functions': functions, 'imports': imports,
                 'data': recovery['data'], 'pointer_relocations': recovery['pointer_relocations'],
                 'data_bindings': data_bindings, 'target_sets': target_sets, 'lifecycle': {}}
    if has_tls:
        contracts['module_image'] = True
        contracts['tls_model'] = 'general-dynamic'
    elif entry == 0:
        contracts['module_image'] = True
    if target_sets:
        contracts['dispatch'] = True
    init, fini = dynamic['tags'].get('DT_INIT', 0), dynamic['tags'].get('DT_FINI', 0)
    if init or fini:
        contracts['lifecycle'] = {'owner': 'entry', 'init': init, 'fini': fini}
        for address in (init, fini):
            if address and address not in address_index:
                functions.append({'name': f'sub_{address:x}', 'address': address, 'size': code_hi - address,
                                  'uniform': True, 'result': 'void', 'parameters': []})
    return contracts


if __name__ == '__main__':
    import argparse
    import os
    import subprocess
    import sys
    import tempfile
    from pathlib import Path

    parser = argparse.ArgumentParser()
    parser.add_argument('elf', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--starts-recovery', type=Path, help='generate_contracts recovery (run it first)')
    parser.add_argument('--work', type=Path, default=Path(tempfile.mkdtemp(prefix='sample-contracts-')))
    args = parser.parse_args()

    args.work.mkdir(parents=True, exist_ok=True)
    start_sizes = starts(args.elf)
    start_file = args.work / 'starts.txt'
    start_file.write_text('\n'.join(f'{address:x} recover' for address in sorted(start_sizes)) + '\n')
    for name, empty in (('vtables', {'tables': []}), ('callsites', {'sites': []}), ('jumptables', [])):
        (args.work / f'{name}.json').write_text(json.dumps(empty))
    recovery_path = args.starts_recovery or args.work / 'recovery.json'
    repo = Path(os.environ.get('ANYPS5_AOT_REPO', Path.cwd())).resolve()
    generator = repo / 'tools' / 'native' / 'contracts' / 'generate_contracts.py'
    subprocess.run([sys.executable, str(generator), '--elf', str(args.elf), '--starts', str(start_file),
                    '--vtables', str(args.work / 'vtables.json'), '--callsites', str(args.work / 'callsites.json'),
                    '--jumptables', str(args.work / 'jumptables.json'), '--output', str(recovery_path)],
                   check=True, capture_output=True)
    contracts = build_contracts(args.elf, json.loads(recovery_path.read_text()))
    args.output.write_text(json.dumps(contracts))
    print(json.dumps({'functions': len(contracts['functions']), 'imports': len(contracts['imports']),
                      'data_bindings': len(contracts['data_bindings']), 'target_sets': len(contracts['target_sets'])}))
