#!/usr/bin/env python3
"""Measure per-function native AOT acceptance for a uniform contract set.

Builds the Image, then for every sampled function runs discovery (Function), dataflow
(analyze) and lowering (lower), catching the first rejection. Reports accepted vs rejected
and the top first-rejection reasons. Compiled output is not written; only counts/reasons.

usage: measure_compile.py <elf> <contracts.json> <report.json> [--stride N] [--limit N]
                          [--jobs N]
"""
import argparse
import collections
import json
import multiprocessing
import os
import re

from compiler import Function, Image

PREFIX = re.compile(r'^[^ ]+ at 0x[0-9a-f]+ \([^)]*\): ')
_ADDRESS = re.compile(r'0x[0-9a-f]+')

_IMAGE = None


def reason(error):
    message = _ADDRESS.sub('0xADDR', PREFIX.sub('', str(error)))
    return message.split('\n', 1)[0][:160]


def _init(elf, contracts_path):
    global _IMAGE
    _IMAGE = Image(elf, json.load(open(contracts_path)))


def _measure(spec):
    stage = 'discover'
    try:
        function = Function(_IMAGE, spec)
        stage = 'analyze'
        function.analyze()
        stage = 'lower'
        function.lower()
        return None
    except Exception as error:
        return (f'{stage}: {reason(error)}', f"{spec['name']}: {type(error).__name__}: {error}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('contracts')
    ap.add_argument('report')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--stride', type=int, default=0)
    ap.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1))
    args = ap.parse_args()

    contracts = json.load(open(args.contracts))
    try:
        image = Image(args.elf, contracts)
    except Exception as error:
        print('Image rejected:', error)
        json.dump({'image_rejected': str(error)}, open(args.report, 'w'), indent=1)
        return
    specs = list(image.functions.values())
    if args.stride:
        specs = specs[::args.stride]
    if args.limit:
        specs = specs[:args.limit]
    print(f'image accepted {len(image.functions)} functions; measuring {len(specs)}', flush=True)

    reasons = collections.Counter()
    examples = {}
    accepted = rejected = 0
    if args.jobs > 1:
        with multiprocessing.Pool(args.jobs, initializer=_init, initargs=(args.elf, args.contracts)) as pool:
            for result in pool.imap_unordered(_measure, specs, chunksize=16):
                if result is None:
                    accepted += 1
                else:
                    rejected += 1
                    key, example = result
                    reasons[key] += 1
                    examples.setdefault(key, example)
    else:
        _init(args.elf, args.contracts)
        for spec in specs:
            result = _measure(spec)
            if result is None:
                accepted += 1
            else:
                rejected += 1
                key, example = result
                reasons[key] += 1
                examples.setdefault(key, example)

    report = {'functions': len(specs), 'accepted': accepted, 'rejected': rejected,
              'accept_rate': round(accepted / len(specs), 4) if specs else 0,
              'top_reasons': [{'reason': key, 'count': count, 'example': examples[key]}
                              for key, count in reasons.most_common(40)]}
    json.dump(report, open(args.report, 'w'), indent=1)
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
