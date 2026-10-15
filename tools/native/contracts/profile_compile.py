#!/usr/bin/env python3
"""Profile per-function native IR lowering on a sample of the title's contracts.

usage: profile_compile.py <elf> <contracts.json> [--start N] [--count N] [--top N]

Prints the wall time, seconds/function, and the cProfile hot spots. With --hash it prints a
sha256 over the concatenated IR of the sample (used to prove optimizations are output-neutral).
"""
import argparse
import cProfile
import hashlib
import json
import pstats
import time

from compiler import Function, Image
import registers


def sample_specs(contracts, start, count):
    specs = sorted(contracts['functions'], key=lambda spec: spec['address'])
    return specs[start:start + count]


def run(image, specs):
    digest = hashlib.sha256()
    lowered = 0
    for spec in specs:
        try:
            function = Function(image, spec)
            function.analyze()
            ir = function.lower()
            digest.update(ir.encode())
            lowered += 1
        except Exception:
            continue
    return digest.hexdigest(), lowered


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('contracts')
    ap.add_argument('--start', type=int, default=0)
    ap.add_argument('--count', type=int, default=500)
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--hash', action='store_true')
    ap.add_argument('--profile', action='store_true')
    args = ap.parse_args()
    contracts = json.load(open(args.contracts))
    image = Image(args.elf, contracts)
    registers.prepare(image)
    specs = sample_specs(contracts, args.start, args.count)
    started = time.perf_counter()
    if args.profile:
        profiler = cProfile.Profile()
        profiler.enable()
        digest, lowered = run(image, specs)
        profiler.disable()
        elapsed = time.perf_counter() - started
        stats = pstats.Stats(profiler)
        stats.sort_stats('tottime').print_stats(args.top)
    else:
        digest, lowered = run(image, specs)
        elapsed = time.perf_counter() - started
    print(json.dumps({'functions': len(specs), 'lowered': lowered, 'seconds': round(elapsed, 3),
                      'seconds_per_function_ms': round(1000 * elapsed / max(1, len(specs)), 3),
                      'ir_sha256': digest}))


if __name__ == '__main__':
    main()
