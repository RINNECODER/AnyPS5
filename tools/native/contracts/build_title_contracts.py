#!/usr/bin/env python3
"""Build an Image-valid uniform contract set for a title from the lane's recovery + ABI.

Internal functions become uniform (`guest_calling_convention` x86_64-uniform); imports keep
their verified typed ABI, or become a Rosetta-style return-0 stub when no ABI is known;
undefined data symbols get a zero-filled native binding. Bodies use a control-flow extent
capped at the next strong function entry, so interior labels no longer truncate a body.
Title-derived input and output stay in the title work dir.

usage: build_title_contracts.py --elf eboot.elf --recovery eboot.recovery.json
       --abi eboot.abi.json --gap-ranges eboot.gap-ranges.txt
       --gap-functions eboot.gap-functions.json --fde-starts eboot.fde-starts.txt
       --output title.contracts.json
"""
import argparse
import bisect
import hashlib
import json
import re

from elftools.elf.elffile import ELFFile
from dynamic import parse_dynamic

R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT = 1, 6, 7


STRONG_EVIDENCE = {'call-target', 'prologue'}

# The title's FP instructions are lowered for a fixed host profile (round-to-nearest-even,
# subnormals preserved, exceptions masked, status flags and NaN payloads unobserved). The
# compiler validates the host FPCR at the entry of every function that uses one.
FLOATING_POINT = {'rounding': 'nearest-even', 'subnormals': 'preserve', 'exceptions': 'masked',
                  'status_flags': 'unobserved', 'nan_payloads': 'unobserved'}


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
    ap.add_argument('--gap-ranges', required=True)
    ap.add_argument('--gap-functions', required=True)
    ap.add_argument('--fde-starts', required=True)
    ap.add_argument('--output', required=True)
    args = ap.parse_args()

    raw = open(args.elf, 'rb').read()
    recovery = json.load(open(args.recovery))
    annotations = json.load(open(args.abi))
    target_sets = recovery['target_sets']

    with open(args.elf, 'rb') as handle:
        elf = ELFFile(handle)
        loads = [segment for segment in elf.iter_segments() if segment['p_type'] == 'PT_LOAD']

        def read_mapped(address, size):
            segment = next(item for item in loads
                           if item['p_vaddr'] <= address and address + size <= item['p_vaddr'] + item['p_filesz'])
            elf.stream.seek(segment['p_offset'] + address - segment['p_vaddr'])
            return elf.stream.read(size)

        dynamic = parse_dynamic(elf, read_mapped)
    executable = [segment for segment in loads if segment['p_flags'] & 1]
    code_lo = min(segment['p_vaddr'] for segment in executable)
    code_hi = max(segment['p_vaddr'] + segment['p_filesz'] for segment in executable)
    init, fini = dynamic['tags'].get('DT_INIT', 0), dynamic['tags'].get('DT_FINI', 0)

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

    addresses = sorted(spec['address'] for spec in recovery['functions'])
    address_index = {address: index for index, address in enumerate(addresses)}

    def body_end(address):
        index = address_index[address]
        return addresses[index + 1] if index + 1 < len(addresses) else code_hi

    function_addresses = set(addresses)
    function_addresses.update(address for address in (0x80, init, fini) if address)
    filtered_sets = [sorted({target for target in targets} & function_addresses)
                     for targets in target_sets]
    merged = {}
    for spec in recovery['functions']:
        address = spec['address']
        entry = merged.setdefault(address, {'size': body_end(address) - address, 'calls': [], 'jumps': []})
        entry['calls'].extend(spec.get('indirect_calls', []))
        entry['jumps'].extend(spec.get('indirect_jumps', []))
    for address in function_addresses - set(merged):
        merged[address] = {'size': body_end(address) - address if address in address_index else 1,
                           'calls': [], 'jumps': []}
    functions = []
    for address in sorted(merged):
        entry = merged[address]
        calls = [{'address': call['address'], 'target_set': call['target_set']}
                 for call in entry['calls'] if filtered_sets[call['target_set']]]
        jumps = [{'address': jump['address'], 'targets': target_sets[jump['target_set']]}
                 for jump in entry['jumps'] if target_sets[jump['target_set']]]
        functions.append({'name': f'sub_{address:x}', 'address': address, 'size': entry['size'],
                          'uniform': True, 'result': 'void', 'parameters': [],
                          'indirect_calls': calls, 'indirect_jumps': jumps})

    lifecycle = {}
    if init or fini:
        lifecycle = {'owner': 'entry', 'init': init, 'fini': fini}

    contracts = {'schema': 1, 'sha256': hashlib.sha256(raw).hexdigest(),
                 'max_segment_bytes': 256 * 1024 * 1024,
                 'floating_point': FLOATING_POINT,
                 'allow_unlowered_tls': True,
                 'dispatch': True,
                 'functions': functions, 'imports': imports,
                 'data': recovery['data'], 'pointer_relocations': recovery['pointer_relocations'],
                 'data_bindings': data_bindings, 'target_sets': filtered_sets, 'lifecycle': lifecycle}
    json.dump(contracts, open(args.output, 'w'), separators=(',', ':'))
    print(json.dumps({'functions': len(functions), 'imports': len(imports),
                      'stub_imports': sum(1 for contract in imports.values() if contract.get('stub')),
                      'data_bindings': len(data_bindings),
                      'indirect_call_sites': sum(len(spec['indirect_calls']) for spec in functions),
                      'indirect_jump_sites': sum(len(spec['indirect_jumps']) for spec in functions),
                      'lifecycle': lifecycle}, indent=1))


if __name__ == '__main__':
    main()
