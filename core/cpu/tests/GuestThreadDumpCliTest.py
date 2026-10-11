"""Stall diagnostics: ANYPS5_GUEST_THREAD_DUMP_MS emits guest_threads events naming each guest thread."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from CliDiagnostics import parse_diagnostics


runner, main_fixture, guest_fixture = (Path(value).resolve() for value in sys.argv[1:4])


def run(directory, period):
    environment = dict(os.environ)
    environment.pop("ANYPS5_GUEST_THREAD_DUMP_MS", None)
    if period is not None:
        environment["ANYPS5_GUEST_THREAD_DUMP_MS"] = period
    result = subprocess.run(
        [str(runner), "--diagnostics-json", "--unresolved-import-return", "42", "--sce-module", "ManyImportsGuest.prx",
         str(directory / main_fixture.name), "42"],
        cwd=directory, capture_output=True, text=True, timeout=60, env=environment)
    assert result.returncode == 0 and result.stdout == "", (result.returncode, result.stdout, result.stderr[-4000:])
    events = parse_diagnostics(result.stderr)
    return [event for event in events if event["event"] == "guest_threads"]


with tempfile.TemporaryDirectory(prefix="anyps5 thread dump ") as name:
    directory = Path(name)
    shutil.copyfile(main_fixture, directory / main_fixture.name)
    shutil.copyfile(guest_fixture, directory / "ManyImportsGuest.prx")

    assert run(directory, None) == [], "thread snapshots were emitted without a request"
    print("no dump request emits no guest_threads event PASS")

    # A period longer than the run still records the first scheduler turn.
    dumps = run(directory, "600000")
    assert len(dumps) == 1, dumps
    threads = dumps[0]["threads"]
    assert [thread["thread_id"] for thread in threads] == [0x10001], threads
    initial = threads[0]
    for key, kind in (("name", str), ("state", str), ("active", bool), ("queued", bool), ("policy", int),
                      ("priority", int), ("rip", str), ("rsp", str), ("slices", int), ("instructions", int),
                      ("last_import", str), ("host_entries", int), ("wait_domain", int), ("wait_key", str),
                      ("frames", list)):
        assert type(initial[key]) is kind, (key, initial)
    assert initial["state"] == "runnable" and initial["queued"] and initial["slices"] == 0, initial
    assert re.fullmatch(r"0x[0-9a-f]+", initial["rsp"]), initial
    print("a requested period records every guest thread at the first scheduler turn PASS")
