"""SIGINT/SIGTERM stop anyps5_cpu_run through its clean stop path.

The guest is a one-instruction spin loop (`jmp .`) in a minimal static ELF,
so it is still executing when the signal arrives. A clean stop exits with
128 + signal and a structured `interrupted` error event; the default kill
(no handler) or an abort (std::terminate) fails.
"""
import json
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time


def spin_elf(path):
    base, offset, code = 0x400000, 0x1000, b'\xeb\xfe'
    header = struct.pack('<16sHHIQQQIHHHHHH', b'\x7fELF\x02\x01\x01' + bytes(9), 2, 62, 1,
                         base + offset, 64, 0, 0, 64, 56, 1, 64, 0, 0)
    program = struct.pack('<IIQQQQQQ', 1, 5, 0, base, base, offset + len(code), offset + len(code), 0x1000)
    image = header + program
    path.write_bytes(image + bytes(offset - len(image)) + code)


def events(stderr):
    return [json.loads(line) for line in stderr.splitlines() if line.startswith('{')]


def stop(runner, guest, signum):
    process = subprocess.Popen([runner, '--diagnostics-json', guest], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        started = json.loads(process.stderr.readline())
        assert started['event'] == 'startup', started
        time.sleep(0.3)
        assert process.poll() is None, ('Spin guest exited before the signal', process.returncode)
        sent = time.monotonic()
        process.send_signal(signum)
        stdout, stderr = process.communicate(timeout=30)
        elapsed = time.monotonic() - sent
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    name = signal.Signals(signum).name
    status = 128 + signum
    assert process.returncode == status, (name, process.returncode, stderr)
    assert stdout == '', stdout
    rows = events(stderr)
    assert rows and rows[-1] == {'schema_version': 1, 'event': 'error', 'code': 'interrupted',
                                 'message': f'Stopped by {name}', 'executable': str(guest),
                                 'process_exit': status}, rows
    assert 'terminate' not in stderr, stderr
    assert elapsed < 10, (name, elapsed)


def main():
    runner = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='anyps5-stop-signal-') as directory:
        guest = Path(directory) / 'spin.elf'
        spin_elf(guest)
        for signum in (signal.SIGINT, signal.SIGTERM):
            stop(runner, guest, signum)
    print('anyps5_cpu_run SIGINT/SIGTERM: clean requested stop, exit 128+signal, interrupted event PASS')


if __name__ == '__main__':
    assert len(sys.argv) == 2, 'Usage: CliStopSignalTest.py anyps5_cpu_run'
    main()
