#!/usr/bin/env python3
"""Build an Image-valid uniform contract set for a title from the lane's recovery + ABI.

Internal functions become uniform (`guest_calling_convention` x86_64-uniform); imports keep
their verified typed ABI, or become a Rosetta-style return-0 stub when no ABI is known;
undefined data symbols get a zero-filled native binding. Title-derived input and output
stay in the title work dir.

usage: build_title_contracts.py --elf eboot.elf --recovery eboot.recovery.json
       --abi eboot.abi.json --output title.contracts.json
"""
import argparse
import hashlib
import json
import re
import sys

from elftools.elf.elffile import ELFFile
from dynamic import parse_dynamic

R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT = 1, 6, 7


def sanitize(name, used):
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--recovery', required=True)
    ap.add_argument('--abi', required=True)
    ap.add_argument('--output', required=True)
    args = ap.parse_args()

    raw = open(args.elf, 'rb').read()
    recovery = json.load(open(args.recovery))
    annotations = json.load(open(args.abi))
    target_sets = recovery['target_sets']
    function_addresses = {f['address'] for f in recovery['functions']}
    filtered_sets = [sorted(target for target in targets if target in function_addresses)
                     for targets in target_sets]

    with open(args.elf, 'rb') as handle:
        elf = ELFFile(handle)
        loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']

        def read_mapped(address, size):
            segment = next(s for s in loads
                           if s['p_vaddr'] <= address and address + size <= s['p_vaddr'] + s['p_filesz'])
            elf.stream.seek(segment['p_offset'] + address - segment['p_vaddr'])
            return elf.stream.read(size)

        dynamic = parse_dynamic(elf, read_mapped)
    symbols = dynamic['symbols']
    function_imports, data_imports = {}, {}
    for relocation in dynamic['relocations']:
        symbol = symbols[relocation['symbol_index']]
        name = symbol['name']
        if relocation['kind'] in (R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT) and symbol['info'] & 15 != 1:
            function_imports[name] = None
        elif relocation['kind'] in (R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT):
            data_imports[name] = None

    used = set()
    imports = {}
    for name in function_imports:
        record = annotations['imports'].get(name, {})
        if record.get('status') == 'verified':
            imports[name] = {'name': sanitize(record.get('hle_name') or name, used),
                             'result': record['result'], 'parameters': record['parameters']}
        else:
            imports[name] = {'name': sanitize(name, used), 'result': 'i64', 'parameters': [], 'stub': True}
    data_bindings = {name: {'name': sanitize('data_' + name, used), 'size': 4096} for name in data_imports}

    functions = []
    for spec in recovery['functions']:
        address, size = spec['address'], spec['size']
        if size <= 0:
            continue
        calls = []
        for call in spec.get('indirect_calls', []):
            if filtered_sets[call['target_set']]:
                calls.append({'address': call['address'], 'target_set': call['target_set']})
        functions.append({'name': f'sub_{address:x}', 'address': address, 'size': size,
                          'uniform': True, 'result': 'void', 'parameters': [], 'indirect_calls': calls})

    lifecycle = {}
    init, fini = dynamic['tags'].get('DT_INIT', 0), dynamic['tags'].get('DT_FINI', 0)
    if init or fini:
        lifecycle = {'owner': 'entry', 'init': init, 'fini': fini}

    contracts = {'schema': 1, 'sha256': hashlib.sha256(raw).hexdigest(),
                 'max_segment_bytes': 256 * 1024 * 1024,
                 'allow_unlowered_tls': True,
                 'functions': functions, 'imports': imports,
                 'data': recovery['data'], 'pointer_relocations': recovery['pointer_relocations'],
                 'data_bindings': data_bindings, 'target_sets': filtered_sets, 'lifecycle': lifecycle}
    json.dump(contracts, open(args.output, 'w'), separators=(',', ':'))
    print(json.dumps({'functions': len(functions), 'imports': len(imports),
                      'stub_imports': sum(1 for c in imports.values() if c.get('stub')),
                      'data_bindings': len(data_bindings), 'indirect_call_sites': sum(len(f['indirect_calls']) for f in functions),
                      'lifecycle': lifecycle}, indent=1))


if __name__ == '__main__':
    main()
