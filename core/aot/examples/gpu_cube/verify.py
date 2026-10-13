import argparse
import hashlib
import json
import os
import re
import subprocess
import time
import tempfile
from pathlib import Path

from images import compare_frames


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def run(binary, folder, reference):
    folder.mkdir(parents=True, exist_ok=False)
    architecture = subprocess.check_output(['xcrun', 'lipo', '-archs', str(binary)], text=True).strip()
    require(architecture == 'arm64', 'graphics executable must be ARM64-only')
    meta = folder / 'app0/sce_sys'
    meta.mkdir(parents=True)
    (meta / 'param.json').write_text(json.dumps({'titleId': 'PPSA99999', 'localizedParameters': {
        'defaultLanguage': 'en-US', 'en-US': {'titleName': 'Native ARM64 graphics comparison'}}}))
    environment = os.environ.copy()
    unexpected = [k for k in environment if k.startswith('APS5_')]
    require(not unexpected, 'unset diagnostic overrides before acceptance: ' + ', '.join(unexpected))
    environment.update(APS5_DUMP_FRAMES='8', APS5_DUMP_FRAMES_EVERY='75', APS5_DUMP_SCALE='1',
                       APS5_CAPTURE_TRACE='1', ANYPS5_REQUIRE_VULKAN='1')
    start = time.monotonic()
    with (folder / 'execution.log').open('wb') as log:
        result = subprocess.run([str(binary)], cwd=folder, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=30)
    output = (folder / 'execution.log').read_text(errors='replace')
    require(result.returncode == 0, f'graphics executable exited with {result.returncode}; see {folder / "execution.log"}')
    require('architecture=arm64 translated=0' in output, 'native execution marker missing')
    require('[notification] mesh_test: fin' in output, '600-frame source loop did not finish')
    require('native GPU guest teardown completed' in output, 'guest teardown did not run')
    require('[gpu] skipped draw:' not in output, 'driver skipped a draw')
    trace = (folder / 'capture-trace.log').read_text()
    flips = [int(v) for v in re.findall(r'display-flip-complete output=\d+ buffer=\d+ argument=(\d+)', trace)]
    require(flips == list(range(600)), 'completed flip sequence does not match source frames 0 through 599')
    return {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'reference': reference,
            'seconds': time.monotonic() - start, 'completed_flips': len(flips), 'architecture': architecture}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aot', type=Path)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--paths', type=Path)
    outputs = parser.add_mutually_exclusive_group(required=True)
    outputs.add_argument('--output', type=Path)
    outputs.add_argument('--output-parent', type=Path)
    args = parser.parse_args()
    if args.paths:
        require(not (args.aot or args.reference or args.build), '--paths cannot be combined with explicit artifact paths')
        paths = json.loads(args.paths.read_text())
        args.aot, args.reference, args.build = (Path(paths[key]) for key in ('aot', 'reference', 'build'))
    require(args.aot and args.reference and args.build, 'both binaries and build manifest are required')
    if args.output_parent:
        args.output_parent.mkdir(parents=True, exist_ok=True)
        args.output = Path(tempfile.mkdtemp(prefix='run-', dir=args.output_parent))
    else:
        require(not args.output.exists(), 'acceptance output must be fresh')
        args.output.mkdir(parents=True)
    manifest = json.loads((args.build / 'build.json').read_text())
    for name, expected in manifest['hashes'].items():
        require(hashlib.sha256((args.build / name).read_bytes()).hexdigest() == expected, 'build artifact changed: ' + name)
    links = []
    for binary, name in ((args.aot, 'module.o'), (args.reference, 'reference.o')):
        link = json.loads(binary.with_suffix('.link.json').read_text())
        require(hashlib.sha256(binary.read_bytes()).hexdigest() == link['binary_sha256'], 'executable changed after linking')
        require(link['object_sha256'] == manifest['hashes'][name], 'executable belongs to a different object build')
        require(link['build_manifest_sha256'] == hashlib.sha256((args.build / 'build.json').read_bytes()).hexdigest(),
                'executable belongs to a different source manifest')
        links.append(link)
    require(links[0]['runtime_archives'] == links[1]['runtime_archives'], 'AOT and reference use different runtimes')
    runs = [run(args.aot.resolve(), args.output / 'aot', False), run(args.reference.resolve(), args.output / 'reference', True)]
    comparison = compare_frames(args.output / 'aot', args.output / 'reference')
    report = {'build': manifest, 'links': links, 'runs': runs, 'comparison': comparison}
    (args.output / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
    require(comparison['passed'], 'graphics reference comparison failed; see acceptance.json')
    print(args.output / 'acceptance.json')


if __name__ == '__main__':
    main()
