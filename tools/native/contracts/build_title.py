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
import subprocess
import time
from pathlib import Path

import abi
import checked_pointers
import finite_tables
import registers
from compiler import Function, Image, compile_image, stub_function

# The preamble pass skips discovery and dataflow, so the declares only those passes can reach are
# added here for the chunk modules.
EXTRA_DECLARATIONS = [
    'declare void @llvm.trap()',
    'declare void @NativeAotReportUnimplemented_nid_no_patch(i64)',
    'declare void @NativeAotReportGuardFailure_nid_no_patch(i64, i32)',
    'declare ptr @NativeAotDispatch_nid_no_patch(i64)',
    'declare void @NativeAotReportDispatchFailure_nid_no_patch(i64, i64)',
    f'declare zeroext i1 @{checked_pointers.SYMBOL}(ptr, i64, i1 zeroext)',
]

_ADDRESS = re.compile(r'0x[0-9a-f]+')
_PREFIX = re.compile(r'^[^ ]+ at 0x[0-9a-f]+ \([^)]*\): ')
IMAGE = None
NAMES = None


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


def clang_command():
    import os
    import platform
    if os.environ.get('ANYPS5_AOT_CLANG'):
        return [os.environ['ANYPS5_AOT_CLANG']]
    if platform.system() == 'Darwin':
        return ['xcrun', 'clang', '-arch', 'arm64']
    return ['clang']


def import_declarations(image):
    """Declarations for every import, so a chunk module can call a stubbed one it did not define."""
    lines = []
    for contract in image.imports.values():
        result, parameters = abi.signature(contract)
        arguments = ', '.join(f'{abi.parameter_type(t)} %arg{i}' for i, t in enumerate(parameters))
        suffix = ' noreturn' if contract.get('noreturn') else ''
        lines.append(f'declare {abi.result_type(result)} @{contract["name"]}({arguments}){suffix}')
    return lines


def function_declaration(spec):
    """The declaration a module needs for a function it references but does not define."""
    if abi.uniform(spec):
        return f'declare void @{spec["name"]}(ptr)'
    result, parameters = abi.signature(spec)
    arguments = ', '.join(abi.parameter_type(t) for t in parameters)
    suffix = ' noreturn' if spec.get('noreturn') else ''
    return f'declare {abi.result_type(result)} @{spec["name"]}({arguments}){suffix}'


def module_chunk(job):
    """Lower one chunk and write its self-contained module, optionally compiled to an object.

    The chunk carries its own copy of the shared membership tables and helper, so the module has
    no cross-chunk reference and the linker never has to merge a private symbol.
    """
    specs, index, folder, header, compile_object, fingerprint = job
    source = folder / f'chunk_{index:04d}.ll'
    target = source.with_suffix('.o')
    cache = folder / f'chunk_{index:04d}.json'
    if compile_object and target.exists() and cache.exists():
        saved = json.loads(cache.read_text())
        # The chunk cache is keyed by index, so a compiler change must invalidate it: only a chunk
        # built by the same compiler sources as this run can be resumed.
        if saved.get('fingerprint') == fingerprint:
            return index, saved['records'], saved['ir_bytes'], target.stat().st_size
    records, bodies = [], []
    for spec in specs:
        record = compile_one(spec, True)
        body = record.pop('ir', None)
        records.append(record)
        if body is not None:
            bodies.append(body)
        else:
            # A rejected function still needs a body: the dispatch table names every function.
            bodies.append(stub_function(spec, record['reason']))
    tables = finite_tables.module_declarations(IMAGE)
    # A body or a target table can name a function defined by another chunk; declare the ones this
    # chunk references and does not define.
    defined = {spec['name'] for spec in specs}
    referenced = set(re.findall(r'@([A-Za-z_][A-Za-z_0-9]*)', '\n'.join([*bodies, *tables])))
    declarations = [function_declaration(NAMES[name]) for name in sorted(referenced)
                    if name in NAMES and name not in defined]
    text = '\n\n'.join([header[0], *declarations, *header[1:], *bodies, *tables]) + '\n'
    source = folder / f'chunk_{index:04d}.ll'
    source.write_text(text)
    object_bytes = 0
    if compile_object:
        command = ['nice', '-n', '10', *clang_command(), '-O3', '-c', str(source), '-o', str(target)]
        # A compile can fail transiently under machine memory pressure; retry once before giving up.
        for attempt in (1, 2):
            result = subprocess.run(command, capture_output=True)
            if result.returncode == 0:
                break
            if attempt == 2:
                raise subprocess.CalledProcessError(result.returncode, command, result.stdout, result.stderr)
        object_bytes = target.stat().st_size
        source.unlink()
    cache.write_text(json.dumps({'fingerprint': fingerprint, 'ir_bytes': len(text), 'records': records}))
    return index, records, len(text), object_bytes


def build_modules(args):
    """Emit a preamble module plus one module per chunk, compile each, and link one image."""
    folder = args.objects
    folder.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    global NAMES
    NAMES = {spec['name']: spec for spec in IMAGE.functions.values()}
    preamble = folder / 'preamble.ll'
    pieces = []
    compile_image(IMAGE, sink=pieces.append, emit_bodies=False, analyze=False)
    # The dispatch table names every function, so the preamble declares them all, after the
    # target triple and before the table that references them.
    triple, rest = ''.join(pieces).split('\n', 1)
    declarations = '\n'.join(function_declaration(spec) for spec in IMAGE.functions.values())
    preamble.write_text(f'{triple}\n{declarations}\n\n{rest}')
    header = list(dict.fromkeys([*IMAGE.module_header, *EXTRA_DECLARATIONS, *IMAGE.module_externs]))
    # Stub imports are defined (not declared) by the preamble, so a chunk that calls one needs its
    # own declaration; a name the preamble already declared must not be repeated.
    declared = {match.group(1) for line in header
                if (match := re.match(r'declare .*?@([A-Za-z_][A-Za-z_0-9]*)\(', line))}
    for line in import_declarations(IMAGE):
        name = re.match(r'declare .*?@([A-Za-z_][A-Za-z_0-9]*)\(', line).group(1)
        if name not in declared:
            declared.add(name)
            header.append(line)
    chunks = [args.specs[index:index + args.chunk] for index in range(0, len(args.specs), args.chunk)]
    records, ir_bytes, object_bytes = [], 0, 0
    context = multiprocessing.get_context('fork')
    fingerprint_value = fingerprint()
    with context.Pool(args.jobs) as pool:
        jobs = [(chunk, index, folder, header, not args.no_object, fingerprint_value)
                for index, chunk in enumerate(chunks)]
        for _, result, chunk_ir, chunk_object in pool.imap_unordered(module_chunk, jobs):
            records.extend(result)
            ir_bytes += chunk_ir
            object_bytes += chunk_object
    preamble_object = 0
    if not args.no_object:
        target = preamble.with_suffix('.o')
        subprocess.run(['nice', '-n', '10', *clang_command(), '-O0', '-c', str(preamble), '-o', str(target)],
                       check=True, capture_output=True)
        preamble_object = target.stat().st_size
        objects = sorted(folder.glob('chunk_*.o'))
        image = folder / 'image.o'
        subprocess.run(['xcrun', 'ld', '-r', '-arch', 'arm64', '-o', str(image),
                        str(target), *map(str, objects)], check=True, capture_output=True)
    wall = time.perf_counter() - started
    accepted = [record for record in records if record['status'] == 'ok']
    reasons = {}
    for record in records:
        if record['status'] == 'rejected':
            reasons[record['reason']] = reasons.get(record['reason'], 0) + 1
    summary = {
        'functions': len(records), 'accepted': len(accepted), 'rejected': len(records) - len(accepted),
        'chunks': len(chunks), 'jobs': args.jobs, 'chunk': args.chunk,
        'wall_seconds': round(wall, 2), 'chunk_ir_bytes': ir_bytes + preamble.stat().st_size,
        'preamble_bytes': preamble.stat().st_size, 'object_bytes': object_bytes + preamble_object,
        'image_bytes': (folder / 'image.o').stat().st_size if not args.no_object else 0,
        'top_rejections': [{'reason': key, 'count': count}
                           for key, count in sorted(reasons.items(), key=lambda item: -item[1])[:20]],
    }
    print(json.dumps(summary, indent=2))
    if args.summary:
        args.summary.write_text(json.dumps(summary, indent=2) + '\n')


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
    parser.add_argument('--objects', type=Path,
                        help='partitioned build: emit a preamble module plus one module per chunk, '
                             'compile each and link one image into this directory')
    parser.add_argument('--no-object', action='store_true', help='emit the modules but do not compile or link')
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
    registers.prepare(IMAGE)

    if args.objects:
        args.specs = specs
        return build_modules(args)

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
