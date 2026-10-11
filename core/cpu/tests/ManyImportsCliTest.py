"""A graph with more than a thousand imports loads: resolved services and trap stubs share host gates."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from BuildManyImportsFixture import ABSENT_MODULE, IMPORTS, LIBC_SCOPES, LIBC_SERVICES, UNKNOWN_COUNT, unknown_name
from BuildSceModulesFixture import nid
from CliDiagnostics import parse_diagnostics


runner, main_fixture, guest_fixture = (Path(value).resolve() for value in sys.argv[1:4])
UNKNOWN = [nid(unknown_name(index)) for index in range(UNKNOWN_COUNT)]
assert len(IMPORTS) > 1000 and len(LIBC_SCOPES) * len(LIBC_SERVICES) > 256, "fixture no longer exceeds the limits"


def run(directory, *options, expected="42"):
    result = subprocess.run(
        [str(runner), "--diagnostics-json", *options, "--sce-module", "ManyImportsGuest.prx",
         str(directory / main_fixture.name), expected],
        cwd=directory, capture_output=True, text=True, timeout=60)
    assert result.stdout == "", (result.returncode, result.stdout, result.stderr[-4000:])
    return result, parse_diagnostics(result.stderr)


def tail(result):
    return result.stderr[-4000:]


with tempfile.TemporaryDirectory(prefix="anyps5 many imports ") as name:
    directory = Path(name)
    shutil.copyfile(main_fixture, directory / main_fixture.name)
    shutil.copyfile(guest_fixture, directory / "ManyImportsGuest.prx")

    # Every resolved libc service is called through its own import, then every trap stub
    # returns the configured value and logs its first call.
    result, events = run(directory, "--unresolved-import-return", "42")
    assert result.returncode == 0, (result.returncode, tail(result))
    assert events[-1] == {"schema_version": 1, "event": "guest_exit", "exit_code": 0}, events[-3:]
    bound = [event for event in events if event["event"] == "unresolved_import"]
    called = [event for event in events if event["event"] == "unresolved_import_called"]
    assert sorted(event["nid"] for event in bound) == sorted(UNKNOWN) and {event["binding"] for event in bound} == {"trap"}, bound[:3]
    assert {event["library"] for event in bound} == {ABSENT_MODULE}, bound[:3]
    assert [event["nid"] for event in called] == UNKNOWN, called[:3]
    assert all(event["action"] == "return" and event["return_value"] == 42 for event in called), called[:3]
    print(f"PASS {len(IMPORTS)} imports: {len(LIBC_SCOPES) * len(LIBC_SERVICES)} resolved libc imports "
          f"and {UNKNOWN_COUNT} trap stubs load, run and log each first stub call")

    # A wrong configured value proves the guest observed every stub's return.
    result, events = run(directory, "--unresolved-import-return", "41")
    assert result.returncode == 80 and events[-1]["exit_code"] == 80, (result.returncode, tail(result))
    print("PASS a stub returning an unexpected value is observed by the guest")

    # Without a configured value the first stub call stops the guest naming its NID.
    result, events = run(directory)
    assert result.returncode == 126, (result.returncode, tail(result))
    assert "startup" in [event["event"] for event in events], tail(result)
    first, error = events[-2:]
    assert first["event"] == "unresolved_import_called" and first["nid"] == UNKNOWN[0], events[-2:]
    assert first["action"] == "abort" and error["code"] == "unsupported_service", events[-2:]
    assert UNKNOWN[0] in error["message"] and "library=" + ABSENT_MODULE in error["message"], error
    print("PASS without a configured value the first stub call aborts naming its NID")
