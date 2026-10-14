#!/usr/bin/env python3
"""Chunked whole-title native AOT build with a per-function cache.

Compiles every declared function independently (discovery, dataflow, lowering) in
address-ordered chunks and records a compact per-function result (status, rejection
reason, IR size and sha256, CPU time) in a cache keyed by the contracts image and a
fingerprint of the compiler sources. An interrupted or repeated build only recompiles
the functions missing from the cache. Prints wall and CPU totals for the whole title.

usage: build_title.py <elf> <contracts.json> [--cache DIR] [--jobs N] [--chunk N]
                      [--limit N] [--stride N] [--keep-ir] [--summary FILE]
"""
import argparse
import hashlib
import json
import multiprocessing
import re
import time
from pathlib import Path

from compiler import Function, Image

_ADDRESS = re.compile(r'0x[0-9a-f]+')
_PREFIX = re.compile(r'^[^ ]+ at 0x[0-9a-f]+ \([^)]*\): ')
IMAGE = None


def reason(error):
    return _ADDRESS.sub('0xADDR', _PREFIX.sub('', str(error))).split('\n', 1)[0][:160]


def compile_one(spec, keep_ir):
    started = time.process_time()
    record = {'address': spec['address'], 'name': spec['name']}
    try:
        function = Function(IMAGE, spec)
        function.analyze()
        ir = function.lower()
    except Exception as error:
        record.update(status='rejected', reason=reason(error))
    else:
        record.update(status='ok', ir_size=len(ir), ir_sha256=hashlib.sha256(ir.encode()).hexdigest(),
                      instructions=len(function.instructions), bounded_loops=function.loop_proofs,
                      native_frame_bytes=function.depth)
        if keep_ir:
            record['ir'] = ir
    finally:
        # A worker compiles thousands of functions against one image; the per-owner
        # indirect-call cache would otherwise keep an entry for every owner it has seen.
        IMAGE.call_contracts.clear()
    record['cpu_ms'] = round(1000 * (time.process_time() - started), 3)
    return record


def compile_chunk(job):
    specs, keep_ir = job
    return [compile_one(spec, keep_ir) for spec in specs]


def fingerprint():
    """Hash of the compiler sources, so a compiler change invalidates the cache."""
    root = Path(__file__).resolve().parents[3]
    digest = hashlib.sha256()
    for path in sorted((root / 'core' / 'aot').glob('*.py')):
        digest.update(path.name.encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('elf')
    parser.add_argument('contracts')
    parser.add_argument('--cache', type=Path, default=Path('build-cache'))
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--chunk', type=int, default=512)
    parser.add_argument('--limit', type=int, default=0)
    parser.add_argument('--stride', type=int, default=0)
    parser.add_argument('--keep-ir', action='store_true')
    parser.add_argument('--summary', type=Path)
    args = parser.parse_args()

    global IMAGE
    contracts = json.loads(Path(args.contracts).read_text())
    specs = sorted(contracts['functions'], key=lambda spec: spec['address'])
    if args.stride:
        specs = specs[::args.stride]
    if args.limit:
        specs = specs[:args.limit]
    IMAGE = Image(args.elf, contracts)

    key = hashlib.sha256((str(contracts.get('sha256')) + fingerprint()).encode()).hexdigest()[:16]
    cache = args.cache / key
    cache.mkdir(parents=True, exist_ok=True)
    chunks = [specs[index:index + args.chunk] for index in range(0, len(specs), args.chunk)]
    records, pending = [], []
    for index, chunk in enumerate(chunks):
        path = cache / f'{index:05d}-{len(chunk)}.json'
        if path.exists():
            records.extend(json.loads(path.read_text()))
        else:
            pending.append((chunk, path))

    started = time.perf_counter()
    if pending:
        context = multiprocessing.get_context('fork')
        with context.Pool(args.jobs) as pool:
            jobs = [(chunk, args.keep_ir) for chunk, _ in pending]
            for (_, path), result in zip(pending, pool.imap(compile_chunk, jobs)):
                path.write_text(json.dumps(result))
                records.extend(result)
    wall = time.perf_counter() - started

    accepted = [record for record in records if record['status'] == 'ok']
    rejected = [record for record in records if record['status'] == 'rejected']
    cpu = sum(record['cpu_ms'] for record in records) / 1000
    reasons = {}
    for record in rejected:
        reasons[record['reason']] = reasons.get(record['reason'], 0) + 1
    summary = {
        'functions': len(records), 'accepted': len(accepted), 'rejected': len(rejected),
        'acceptance_rate': round(len(accepted) / max(1, len(records)), 4),
        'wall_seconds': round(wall, 2), 'cpu_seconds': round(cpu, 2),
        'cpu_ms_per_function': round(1000 * cpu / max(1, len(records)), 3),
        'functions_per_second': round(len(records) / wall, 1) if wall else None,
        'jobs': args.jobs, 'chunk': args.chunk, 'cache': str(cache),
        'compiled_chunks': len(pending), 'cached_chunks': len(chunks) - len(pending),
        'ir_bytes': sum(record.get('ir_size', 0) for record in accepted),
        'top_rejections': [{'reason': key, 'count': count}
                           for key, count in sorted(reasons.items(), key=lambda item: -item[1])[:20]],
    }
    print(json.dumps(summary, indent=2))
    if args.summary:
        args.summary.write_text(json.dumps(summary, indent=2) + '\n')


if __name__ == '__main__':
    main()
