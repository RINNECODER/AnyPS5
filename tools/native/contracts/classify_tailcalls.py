#!/usr/bin/env python3
"""Classify 'tail call requires a balanced native frame' rejections at a stride.

For each sampled function that fails with that reason, report whether the tail-call
instruction is a fall-through into the next function (a continuation) or a real
branch (jmp/jcc/call).
"""
import argparse
import collections
import json
import multiprocessing

from compiler import Function, Image

_IMAGE = None


def _init(elf, contracts_path):
    global _IMAGE
    _IMAGE = Image(elf, json.load(open(contracts_path)))


def _one(spec):
    try:
        function = Function(_IMAGE, spec)
        function.analyze()
        function.lower()
        return None
    except Exception as error:
        if 'balanced native frame' not in str(error):
            return None
        kinds = []
        for address in function.tail_calls:
            ins = function.instructions.get(address)
            if ins is None:
                kinds.append('missing')
                continue
            fallthrough = address + ins.size
            kinds.append(f'{ins.mnemonic}:{"fallthrough" if fallthrough == spec["address"] + spec["size"] else "branch"}')
        return (spec['name'], spec['address'], spec['size'], kinds, str(error)[:80])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('contracts')
    ap.add_argument('--stride', type=int, default=50)
    ap.add_argument('--jobs', type=int, default=4)
    args = ap.parse_args()
    contracts = json.load(open(args.contracts))
    specs = sorted(contracts['functions'], key=lambda spec: spec['address'])[::args.stride]
    counts = collections.Counter()
    examples = []
    with multiprocessing.Pool(args.jobs, initializer=_init, initargs=(args.elf, args.contracts)) as pool:
        for result in pool.imap_unordered(_one, specs, chunksize=8):
            if result is None:
                continue
            name, address, size, kinds, error = result
            key = ','.join(kinds) or 'none'
            counts[key] += 1
            if len(examples) < 20:
                examples.append((name, hex(address), hex(size), key, error))
    print('total balanced-frame rejections:', sum(counts.values()))
    for key, count in counts.most_common():
        print(f'{count:5d}  {key}')
    print('--- examples ---')
    for name, address, size, key, error in examples:
        print(name, address, size, key)


if __name__ == '__main__':
    main()
