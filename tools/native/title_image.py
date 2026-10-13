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
import struct
import sys
import time
from collections import deque

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM

from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "core" / "aot"))
from compiler import Image, compile_image, Rejected  # noqa: E402
from toolchain import build_native_object  # noqa: E402


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
    args = parser.parse_args()
    contracts = json.loads(open(args.contracts).read())
    image = Image(args.elf, contracts)
    entries = []
    if 'entry' in args.entries:
        entries.append(image.elf['e_entry'])
    if 'init' in args.entries:
        init = image.contracts.get('lifecycle', {}).get('init')
        if init:
            entries.append(init)
    start = time.time()
    order, frontier = reachable(image, entries, args.limit)
    native = set(order)
    lifecycle = contracts.get('lifecycle', {})
    keep = native | frontier | {lifecycle.get('init'), lifecycle.get('fini')} - {None}
    contracts['functions'] = [spec for spec in image.functions.values() if spec['address'] in keep]
    for spec in contracts['functions']:
        if spec['address'] not in native:
            spec['stub'] = True
    contracts['allow_unimplemented'] = True
    contracts['guest_data'] = True
    image = Image(args.elf, contracts)
    print(f'native {len(order)} stub {len(frontier)} entries {[hex(e) for e in entries]} ({time.time() - start:.1f}s)', flush=True)
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
