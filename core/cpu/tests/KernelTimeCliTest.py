"""libkernel time services through anyps5_cpu_run's real resolver chain (#278).

The synthetic guest calls every clock, counter and sleep through PLT imports in the
libkernel and libScePosix scopes and exits with a nonzero code naming the first broken
contract. The sleep case runs a second guest thread that must keep sampling the clock
while the main thread sleeps; the stop case sends SIGINT to a guest parked in an hour
long sceKernelSleep and expects the clean interrupted exit, not a wait for the hour.
"""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from CliDiagnostics import parse_diagnostics


runner, main_fixture, guest_fixture = (Path(value).resolve() for value in sys.argv[1:4])


def command(main, module, *arguments):
    return [str(runner), "--diagnostics-json", "--sce-module", module, str(main), *arguments]


def run(cwd, main, module, *arguments):
    result = subprocess.run(command(main, module, *arguments), cwd=cwd, capture_output=True, text=True, timeout=60)
    assert result.stdout == "", (result.returncode, result.stdout, result.stderr)
    events = parse_diagnostics(result.stderr)
    called = [event for event in events if event["event"] == "unresolved_import_called"]
    assert not called, ("guest called an unimplemented import", called, result.stderr)
    assert result.returncode == 0, (arguments[0], "guest failure code", result.returncode, result.stderr[-4000:])
    assert events[-1] == {"schema_version": 1, "event": "guest_exit", "exit_code": 0}, events
    return events


with tempfile.TemporaryDirectory(prefix="anyps5 kernel time ") as directory:
    root = Path(directory)
    images, cwd = root / "guest images", root / "unrelated cwd"
    images.mkdir()
    cwd.mkdir()
    main = images / main_fixture.name
    guest = images / "KernelTimeGuest.prx"
    shutil.copyfile(main_fixture, main)
    shutil.copyfile(guest_fixture, guest)
    module = os.path.relpath(guest, cwd)

    events = run(cwd, main, module, "clocks", str(int(time.time())))
    bound = {event["nid"] for event in events if event["event"] == "unresolved_import"}
    assert not bound, ("time imports were bound to trap stubs", bound)
    print("PASS clocks: process time, counter and TSC agree on elapsed time; every clock id, "
          "gettimeofday, UTC conversion and SCE/POSIX error conventions")

    started = time.monotonic()
    run(cwd, main, module, "sleeps")
    elapsed = time.monotonic() - started
    assert elapsed >= 2.1, ("sleeps returned before the requested time", elapsed)
    print(f"PASS sleeps: sleeping thread parked while the other guest thread ran; "
          f"usleep/nanosleep/sleep in both scopes ({elapsed:.2f} s)")

    process = subprocess.Popen(command(main, module, "forever"), cwd=cwd, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        while True:
            line = process.stderr.readline()
            assert line, ("guest exited before startup", process.wait(), process.stderr.read())
            if line.startswith("{") and json.loads(line)["event"] == "startup":
                break
        time.sleep(0.5)
        assert process.poll() is None, ("forever guest exited before the signal", process.returncode)
        sent = time.monotonic()
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=30)
        stopped = time.monotonic() - sent
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    assert process.returncode == 128 + signal.SIGINT, (process.returncode, stderr)
    assert stdout == "", stdout
    rows = parse_diagnostics(stderr)
    assert rows and rows[-1]["event"] == "error" and rows[-1]["code"] == "interrupted", rows
    assert stopped < 10, ("a stop request waited for the guest sleep", stopped)
    print(f"PASS stop: SIGINT ends a guest parked in sceKernelSleep(3600) in {stopped:.2f} s")
