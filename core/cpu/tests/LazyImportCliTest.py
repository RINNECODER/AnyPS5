"""Unresolved imports bind to trap stubs at load; strict mode keeps the old fatal load."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from BuildLazyImportFixture import ABSENT_MODULE
from BuildSceModulesFixture import nid
from CliDiagnostics import parse_diagnostics


runner, main_fixture, guest_fixture = (Path(value).resolve() for value in sys.argv[1:4])
ABSENT_CALL, ABSENT_WEAK, NEVER_CALLED = (nid(name) for name in (
    "anyps5LazyAbsentCall", "anyps5LazyAbsentWeak", "anyps5LazyNeverCalled"))


def run(cwd, main, module, *options, expected="42"):
    result = subprocess.run(
        [str(runner), "--diagnostics-json", *options, "--sce-module", module, str(main), expected],
        cwd=cwd, capture_output=True, text=True, timeout=20)
    assert result.stdout == "", (result.returncode, result.stdout, result.stderr)
    return result, parse_diagnostics(result.stderr)


def unresolved(events):
    return {event["nid"]: event for event in events if event["event"] == "unresolved_import"}


with tempfile.TemporaryDirectory(prefix="anyps5 lazy imports é ") as directory:
    root = Path(directory)
    images, cwd = root / "guest images", root / "unrelated cwd"
    images.mkdir()
    cwd.mkdir()
    main = images / main_fixture.name
    guest = images / "LazyImportGuest.prx"
    shutil.copyfile(main_fixture, main)
    shutil.copyfile(guest_fixture, guest)
    module = os.path.relpath(guest, cwd)

    # Default: the graph loads, offline NP/WebApi providers run, and the first call to
    # an unresolved import stops the guest with a diagnostic naming module, library and NID.
    result, events = run(cwd, main, module)
    assert result.returncode == 126, (result.returncode, result.stderr)
    kinds = [event["event"] for event in events]
    assert kinds[-2:] == ["unresolved_import_called", "error"] and "startup" in kinds, (events, result.stderr)
    assert {"event": "missing_module", "name": ABSENT_MODULE + ".prx"}.items() <= next(
        event for event in events if event["event"] == "missing_module").items(), (events, result.stderr)
    bound = unresolved(events)
    assert bound[NEVER_CALLED]["binding"] == "trap" and bound[NEVER_CALLED]["module"] == "libkernel", bound
    assert bound[ABSENT_CALL]["binding"] == "trap" and bound[ABSENT_CALL]["library"] == ABSENT_MODULE, bound
    assert bound[ABSENT_WEAK]["binding"] == "weak_zero", bound
    assert all(event["consumer"] == main.name for event in bound.values()), bound
    called, error = events[-2:]
    assert called["nid"] == ABSENT_CALL and called["action"] == "abort", called
    assert error["code"] == "unsupported_service" and error["process_exit"] == 126, error
    for part in (ABSENT_CALL, "library=" + ABSENT_MODULE, "module=" + ABSENT_MODULE, "consumer=" + main.name):
        assert part in error["message"], (part, error)
    print("PASS default lazy imports: graph loads past missing module, never-called/weak imports bound, "
          "offline NP/WebApi2/WebApi succeed, called stub aborts naming module/library/NID")

    # A configured return value lets the guest continue through the stub.
    for expected, status in (("42", 0), ("43", 80)):
        result, events = run(cwd, main, module, "--unresolved-import-return", "42", expected=expected)
        assert result.returncode == status, (status, result.returncode, result.stderr)
        kinds = [event["event"] for event in events]
        assert kinds[-1] == "guest_exit" and events[-1]["exit_code"] == status, (events, result.stderr)
        called = [event for event in events if event["event"] == "unresolved_import_called"]
        assert len(called) == 1 and called[0]["nid"] == ABSENT_CALL, called
        assert called[0]["action"] == "return" and called[0]["return_value"] == 42, called
    print("PASS --unresolved-import-return: called stub returns the configured value and the guest exits normally")

    # Strict mode restores the eager fatal load.
    result, events = run(cwd, main, module, "--strict-imports")
    assert result.returncode == 126, (result.returncode, result.stderr)
    assert [event["event"] for event in events] == ["error"], (events, result.stderr)
    assert events[0]["code"] == "loader_failure", events
    assert events[0]["message"] == "SCE module graph: missing DT_NEEDED provider " + ABSENT_MODULE + ".prx", events
    print("PASS --strict-imports: missing provider fails the load before guest entry")

    for options in (["--unresolved-import-return"], ["--unresolved-import-return", "x1"],
                    ["--strict-imports", "--unresolved-import-return", "1"]):
        result, events = run(cwd, main, module, *options)
        assert result.returncode == 126 and events[-1]["code"] == "invalid_arguments", (options, events)
    print("PASS invalid lazy-import options are rejected")
