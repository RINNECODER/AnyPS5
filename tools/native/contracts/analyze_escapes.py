#!/usr/bin/env python3
"""Classify the 'control flow escapes function contract' rejections (ledger n11).

For each sampled function whose discovery escapes its contract, classify the escape
target: (a) the body continues past the contract size (interior label / shared tail),
(b) a tail jump into another function's interior, (c) a noreturn call followed by
data/padding, (d) something else.

usage: analyze_escapes.py <elf> <contracts.json> <report.json> [--stride N] [--jobs N]
"""
import argparse
import bisect
import collections
import json
import multiprocessing
import os
import re

from compiler import Function, Image, Rejected

_IMAGE = None
_STARTS = None


def _init(elf, contracts_path):
    global _IMAGE, _STARTS
    _IMAGE = Image(elf, json.load(open(contracts_path)))
    specs = sorted((s['address'], s['size']) for s in _IMAGE.functions.values())
    _STARTS = specs


def _owner(address):
    starts = [a for a, _ in _STARTS]
    index = bisect.bisect_right(starts, address) - 1
    if index >= 0 and _STARTS[index][0] <= address < _STARTS[index][0] + _STARTS[index][1]:
        return _STARTS[index]
    return None


def _disasm(address):
    decoded = list(_IMAGE.decoder.disasm(_IMAGE.read(address, 15), address, count=1))
    return decoded[0] if decoded else None


def _decodable(address):
    return _disasm(address) is not None


def _classify(spec, target, predecessor):
    start, size = spec['address'], spec['size']
    owner = _owner(target)
    if owner is not None and owner[0] != start:
        return 'b_tail_into_function_interior'
    if predecessor is not None:
        instruction = _disasm(predecessor)
        if instruction is not None and instruction.mnemonic == 'call' and not _decodable(target):
            return 'c_noreturn_call_then_data'
        if instruction is not None:
            return 'a_body_continues_or_branch_beyond'
    return 'd_other'


def _measure(spec):
    try:
        Function(_IMAGE, spec)
        return None
    except Rejected as error:
        match = re.search(r'escapes function contract to 0x([0-9a-f]+) from (\d+|None)', str(error))
        if not match:
            return None
        target = int(match.group(1), 16)
        predecessor = None if match.group(2) == 'None' else int(match.group(2))
        return (_classify(spec, target, predecessor), hex(target), predecessor)
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('contracts')
    ap.add_argument('report')
    ap.add_argument('--stride', type=int, default=500)
    ap.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1))
    args = ap.parse_args()

    contracts = json.load(open(args.contracts))
    image = Image(args.elf, contracts)
    specs = list(image.functions.values())[::args.stride]
    counts = collections.Counter()
    examples = {}
    with multiprocessing.Pool(args.jobs, initializer=_init, initargs=(args.elf, args.contracts)) as pool:
        for result in pool.imap_unordered(_measure, specs, chunksize=16):
            if result is None:
                continue
            kind, target, predecessor = result
            counts[kind] += 1
            examples.setdefault(kind, f'{target} from {predecessor}')
    json.dump({'sampled': len(specs), 'escapes': sum(counts.values()),
               'classes': dict(counts), 'examples': examples},
              open(args.report, 'w'), indent=1)
    print(json.dumps({'sampled': len(specs), 'escapes': sum(counts.values()),
                      'classes': dict(counts), 'examples': examples}, indent=1))


if __name__ == '__main__':
    main()
