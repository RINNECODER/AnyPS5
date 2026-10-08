import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from CliDiagnostics import parse_diagnostics


runner, main_fixture, guest_fixture = (Path(value).resolve() for value in sys.argv[1:4])
rejection = None
if len(sys.argv) != 4:
    if len(sys.argv) != 6 or sys.argv[4] != "--expect-rejection" or sys.argv[5] not in (
            "libSceNpManager.prx", "libSceNet.prx", "libSceCommonDialog.prx"):
        raise SystemExit("Usage: PlatformServiceCliTest.py runner main.elf guest.prx [--expect-rejection host.prx]")
    rejection = sys.argv[5]


with tempfile.TemporaryDirectory(prefix="anyps5 platform graph é ") as directory:
    root = Path(directory)
    images = root / "guest images with spaces"
    cwd = root / "unrelated cwd"
    images.mkdir()
    cwd.mkdir()
    main = images / main_fixture.name
    guest = images / "PlatformServiceGuest.prx"
    shutil.copyfile(main_fixture, main)
    shutil.copyfile(guest_fixture, guest)
    module_argument = os.path.relpath(guest, cwd)
    input_value = 17
    expected = input_value * input_value + 3 * input_value + 7
    cases = [(expected, 0)] if rejection else [(expected, 0), (expected + 1, 77)]
    for expected_math, status in cases:
        result = subprocess.run(
            [str(runner), "--diagnostics-json", "--sce-module", module_argument,
             str(main), str(input_value), str(expected_math)],
            cwd=cwd, capture_output=True, text=True, timeout=20)
        assert result.stdout == "", (result.returncode, result.stdout, result.stderr)
        events = parse_diagnostics(result.stderr)
        if rejection:
            assert result.returncode == 126, (result.returncode, result.stderr)
            assert len(events) == 1 and events[0]["event"] == "error", (events, result.stderr)
            assert events[0]["code"] == "loader_failure" and events[0]["process_exit"] == 126, (events, result.stderr)
            assert events[0]["message"] == "SCE module graph: missing DT_NEEDED provider " + rejection, (events, result.stderr)
        else:
            assert result.returncode == status, (status, result.returncode, result.stderr)
            assert [event["event"] for event in events] == ["startup", "guest_exit"], (events, result.stderr)
            assert events[0]["executable"] == str(main) and events[0]["entry"] > 0, (events, result.stderr)
            assert events[0]["host_architecture"] == "arm64" and events[0]["guest_architecture"] == "x86_64", (events, result.stderr)
            assert events[0]["backend"] == "Modern QEMU TCG x86-64 dynamic translation", (events, result.stderr)
            assert events[1]["exit_code"] == status, (events, result.stderr)

if rejection:
    print("PASS missing production host declaration rejected before guest entry: " + rejection)
else:
    print("PASS production module graph NP offline state, CommonDialog initialize/repeat, Net byte order/IPv4 guards, real guest dependency math, exits0/77")
