#!/usr/bin/env python3
"""Compile a bounded, entry-reachable subset of a title eboot into one native image.

The contract set declares every function of the title. The functions reachable from the ELF entry
and the module initializer (following direct calls, the contracts' indirect-call target sets and
the annotated indirect jumps) are compiled natively; every other function becomes a reporting trap
stub, so a title run reports the first function the lifter cannot lower instead of failing the
build. Title-derived output stays in the lane work dir.

usage: title_image.py --elf eboot.elf --contracts title.contracts.json --output title.ll
       [--object title.o] [--limit N] [--stride N]
"""
import argparse
import json
import sys
import time
from collections import deque

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM

from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "core" / "aot"))
from compiler import Image, compile_image, Rejected  # noqa: E402
from toolchain import build_native_object  # noqa: E402


def title_process_parameters(image):
    """The title's process-parameter block with its allocator table resolved to guest addresses.

    The SCE relative relocations that fill the libc parameters and the allocator replacement table
    are part of the pointer-relocation contract, so ``image.relocations`` resolves them here.
    """
    if not image.process_parameters:
        return None
    address, size = image.process_parameters[0]
    relocations = dict(image.relocations)

    libc = relocations.get(address + 0x38, 0)
    if not libc:
        return None
    replacement = relocations.get(libc + 0x30, 0)
    if not replacement:
        return None
    heap_api = replacement + 0x20
    bindings = []
    # 0..9: the heap API. 10/11: the table's initialize/finalize callbacks.
    for index in range(10):
        target = relocations.get(heap_api + index * 8, 0)
        if target:
            bindings.append({'slot': image.guest_base + heap_api + index * 8,
                             'target': image.guest_base + target, 'index': index})
    for index, offset in ((10, 0x10), (11, 0x18)):
        target = relocations.get(replacement + offset, 0)
        if target:
            bindings.append({'slot': image.guest_base + replacement + offset,
                             'target': image.guest_base + target, 'index': index})
    return {'address': image.guest_base + address, 'bytes': size, 'bindings': bindings}


def reachable(image, entry_addresses, limit):
    starts = {spec['address']: spec for spec in image.functions.values()}
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    seen, order, frontier = set(), [], set()
    pending = deque(address for address in entry_addresses if address in starts)
    seen.update(pending)
    while pending and len(order) < limit:
        address = pending.popleft()
        spec = starts[address]
        order.append(address)
        targets = set()
        for insn in decoder.disasm(image.read(address, spec['size']), address):
            if insn.mnemonic in ('call', 'jmp') and insn.operands and insn.operands[0].type == X86_OP_IMM:
                targets.add(insn.operands[0].imm & 0xFFFFFFFFFFFFFFFF)
        for record in spec.get('indirect_calls', []) + spec.get('indirect_jumps', []):
            targets.update(record.get('targets', []))
        for target in sorted(targets):
            if target in starts and target not in seen:
                seen.add(target)
                pending.append(target)
    native = set(order)
    # Only the callees of the native subset need a stub; the rest of the eboot is out of reach for
    # this prototype, and keeping 277k stubs would make the IR (and clang) unusable.
    for address in order:
        spec = starts[address]
        targets = set()
        for insn in decoder.disasm(image.read(address, spec['size']), address):
            if insn.mnemonic in ('call', 'jmp') and insn.operands and insn.operands[0].type == X86_OP_IMM:
                targets.add(insn.operands[0].imm & 0xFFFFFFFFFFFFFFFF)
        for record in spec.get('indirect_calls', []) + spec.get('indirect_jumps', []):
            targets.update(record.get('targets', []))
        frontier |= (targets - native) & set(starts)
    return order, frontier


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--elf', required=True)
    parser.add_argument('--contracts', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--object')
    parser.add_argument('--limit', type=int, default=600)
    parser.add_argument('--entries', default='entry,init')
    parser.add_argument('--nid-symbols', help='file of runtime symbol names (one per line, underscore stripped)')
    parser.add_argument('--guest-base', type=lambda value: int(value, 0), default=0x140000000,
                        help='slide added to every guest vaddr (the Rosetta relinked base by default)')
    parser.add_argument('--hle-log', action='store_true',
                        help='log every import call and its return through HleLogCall/HleLogReturn')
    args = parser.parse_args()
    contracts = json.loads(open(args.contracts).read())
    # The slide must be set before the first Image so the process-parameter bindings carry it.
    contracts['guest_base'] = args.guest_base
    image = Image(args.elf, contracts)
    entries = []
    if 'entry' in args.entries:
        entries.append(image.elf['e_entry'])
    if 'init' in args.entries:
        init = image.contracts.get('lifecycle', {}).get('init')
        if init:
            entries.append(init)
    process_parameters = title_process_parameters(image)
    if process_parameters:
        # The allocator callbacks are reachable only from data, so they are explicit entry roots;
        # the walk then compiles their callees instead of leaving dispatch misses behind them.
        entries += [binding['target'] - image.guest_base for binding in process_parameters['bindings']]
    start = time.time()
    order, frontier = reachable(image, entries, args.limit)
    native = set(order)
    lifecycle = contracts.get('lifecycle', {})
    # DT_INIT/DT_FINI run before and after the entry, so they are native even when the bounded
    # walk does not reach them.
    native |= {lifecycle.get('init'), lifecycle.get('fini')} - {None}
    keep = native | frontier
    contracts['functions'] = [spec for spec in image.functions.values() if spec['address'] in keep]
    for spec in contracts['functions']:
        if spec['address'] in native:
            spec.pop('stub', None)
        else:
            spec['stub'] = True
    # DT_INIT/DT_FINI are init-array loops; their body can run past the next declared start (a
    # spurious interior label), which makes the compiler reject them for escaping the contract.
    # Extend each lifecycle contract to its decoded body and drop the interior starts it covers.
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    for address in sorted({lifecycle.get('init'), lifecycle.get('fini')} - {None}):
        spec = next((item for item in contracts['functions'] if item['address'] == address), None)
        if spec is None:
            continue
        end = address
        for insn in decoder.disasm(image.read(address, min(0x400, 0x44AC1CC - address)), address):
            end = insn.address + insn.size
            if insn.mnemonic in ('ret', 'ud2', 'int3'):
                break
        if end <= address + spec['size']:
            continue
        spec['size'] = end - address
        dropped = [item['address'] for item in contracts['functions']
                   if item is not spec and address < item['address'] < end]
        contracts['functions'] = [item for item in contracts['functions'] if item['address'] not in dropped]
        print('extended lifecycle 0x%x to 0x%x, dropped %s' % (address, end, [hex(a) for a in dropped]), flush=True)
    contracts['allow_unimplemented'] = True
    contracts['guest_data'] = True
    contracts['count_entries'] = True
    contracts['hle_log'] = args.hle_log
    if process_parameters:
        contracts['title_process_parameters'] = process_parameters
    # HLE NID binding: point a typed import at the runtime's <name>_nid_postfix symbol when
    # that exists (the arm64 libs define every guest entry point with that suffix).
    if args.nid_symbols:
        known = set(open(args.nid_symbols).read().split())
        for contract in contracts['imports'].values():
            if not contract.get('stub') and contract['name'] + '_nid_postfix' in known:
                contract['name'] = contract['name'] + '_nid_postfix'
    image = Image(args.elf, contracts)
    print('native %d stub %d entries %s native_has %s (%.1fs)' % (len(order), len(frontier), [hex(e) for e in entries], [hex(a) in [hex(x) for x in native] for a in entries], time.time() - start), flush=True)
    start = time.time()
    ir, report = compile_image(image)
    open(args.output, 'w').write(ir)
    print(f'ir {len(ir) / 1e6:.1f} MB in {time.time() - start:.1f}s; '
          f'unimplemented {report["unimplemented_functions"]}', flush=True)
    if report['unimplemented_reasons']:
        print('top reasons:', json.dumps(report['unimplemented_reasons'][:5]), flush=True)
    if args.object:
        # The prototype keeps the machine-state allocas the strict SDK-sample guard rejects.
        report['native_object'] = build_native_object(ir, args.object, strict=False)
        print('object', report['native_object'], flush=True)
    open(args.output + '.json', 'w').write(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
