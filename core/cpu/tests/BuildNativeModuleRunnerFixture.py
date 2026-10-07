"""Actual package-shaped CLI oracle; consumes existing public SCE fixtures."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def events(result):
    rows = []
    for line in result.stderr.splitlines():
        # Native diagnostic setup can emit library diagnostics. Only structured
        # CLI events count as the public protocol; no unrelated text is success.
        if line.startswith('{'):
            rows.append(json.loads(line))
    assert result.stdout == '', (result.returncode, result.stdout, result.stderr)
    assert rows and all(row.get('schema_version') == 1 for row in rows), result.stderr
    return rows


def main():
    runner, main_image, module, utility = map(lambda p: Path(p).resolve(), sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='native runner package é ') as temporary:
        root = Path(temporary)
        binary = root / 'bin' / runner.name
        binary.parent.mkdir()
        resources = root / 'fixtures'
        resources.mkdir()
        shutil.copy2(runner, binary)
        guest = root / 'public guest with spaces.elf'
        dep = root / 'SceModuleGuest.prx'
        shutil.copyfile(main_image, guest)
        shutil.copyfile(module, dep)
        args = [str(binary), '--diagnostics-json', '--sce-module', str(dep), str(guest),
                '17', '5', '7', '58', '3366582378']
        rejected = subprocess.run(args, cwd=root, capture_output=True, text=True, timeout=20)
        receipt = events(rejected)
        assert rejected.returncode == 126, ('Inactive native profile accepted missing utility', rejected.returncode, rejected.stderr)
        assert [row['event'] for row in receipt] == ['error'], receipt
        assert 'missing package utility metallib' in receipt[0]['message'], receipt
        shutil.copyfile(utility, resources / 'AnyPS5Utilities.metallib')
        for checksum, status in [('3366582378', 0), ('3366582379', 77)]:
            args[-1] = checksum
            result = subprocess.run(args, cwd=root, capture_output=True, text=True, timeout=40)
            receipt = events(result)
            assert result.returncode == status, (status, result.returncode, result.stderr)
            assert [row['event'] for row in receipt] == ['startup', 'guest_exit'], receipt
            assert receipt[0]['executable'] == str(guest) and receipt[0]['host_architecture'] == 'arm64', receipt
            assert receipt[-1]['exit_code'] == status, receipt
        (resources / 'AnyPS5Utilities.metallib').write_bytes(b'not a Metal library')
        invalid = subprocess.run(args, cwd=root, capture_output=True, text=True, timeout=20)
        receipt = events(invalid)
        assert invalid.returncode == 126 and [row['event'] for row in receipt] == ['error'], (invalid.returncode, invalid.stderr)
        assert 'metallib' in receipt[0]['message'].lower() or 'library' in receipt[0]['message'].lower(), receipt
    print('Actual native Main package resource gate, public init/entry/fini arithmetic exits0/77 and invalid-library unwind PASS; no game qualification')


if __name__ == '__main__':
    assert len(sys.argv) == 5, 'Usage: BuildNativeModuleRunnerFixture.py runner public-main public-module utility'
    main()
