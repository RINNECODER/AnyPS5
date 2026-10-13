#!/usr/bin/env python3
"""Measure per-function native AOT acceptance for a uniform contract set.

Builds the Image, then for every function runs discovery (Function), dataflow
(analyze) and lowering (lower), catching the first rejection. Reports accepted vs
rejected and the top first-rejection reasons. Compiled output is not written; only the
counts and reasons.

usage: measure_compile.py <elf> <contracts.json> <report.json> [--limit N]
"""
import argparse
import collections
import json
import re
import sys

from compiler import Function, Image
import effects

PREFIX = re.compile(r'^[^ ]+ at 0x[0-9a-f]+ \([^)]*\): ')


def reason(error):
    message = str(error)
    message = PREFIX.sub('', message)
    message = re.sub(r'0x[0-9a-f]+', '0xADDR', message)
    return message.split('\n', 1)[0][:160]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('contracts')
    ap.add_argument('report')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--stride', type=int, default=0)
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
    for index, spec in enumerate(specs):
        stage = 'discover'
        try:
            function = Function(image, spec)
            stage = 'analyze'
            function.analyze()
            stage = 'lower'
            function.lower()
            accepted += 1
        except Exception as error:
            rejected += 1
            key = f'{stage}: {reason(error)}'
            reasons[key] += 1
            examples.setdefault(key, f"{spec['name']}: {type(error).__name__}: {error}")
        function = None
        if index and index % 20000 == 0:
            print(f'  {index}/{len(specs)} accepted={accepted} rejected={rejected}', flush=True)

    report = {'functions': len(specs), 'accepted': accepted, 'rejected': rejected,
              'accept_rate': round(accepted / len(specs), 4) if specs else 0,
              'top_reasons': [{'reason': key, 'count': count, 'example': examples[key]}
                              for key, count in reasons.most_common(40)]}
    json.dump(report, open(args.report, 'w'), indent=1)
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
