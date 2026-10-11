"""A guest calling the system, dialog, input and NP leaf libraries gets the upstream results.

The guest (fixtures/LeafSystemLibsHomebrew.c) asserts the status codes, handles and output
bytes upstream core/libs/prx returns and exits with the source line of the first failed
check. Strict imports prove every import resolves and every listed module is provided.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from CliDiagnostics import parse_diagnostics


runner, main_fixture, guest_fixture = (Path(value).resolve() for value in sys.argv[1:4])

with tempfile.TemporaryDirectory(prefix="anyps5 leaf libs ") as directory:
    root = Path(directory)
    images, cwd = root / "guest images", root / "unrelated cwd"
    images.mkdir()
    cwd.mkdir()
    main = images / main_fixture.name
    shutil.copyfile(main_fixture, main)
    shutil.copyfile(guest_fixture, images / guest_fixture.name)
    module = os.path.relpath(images / guest_fixture.name, cwd)
    result = subprocess.run(
        [str(runner), "--diagnostics-json", "--strict-imports", "--sce-module", module, str(main)],
        cwd=cwd, capture_output=True, text=True, timeout=60)
    events = parse_diagnostics(result.stderr)
    assert result.stdout == "", (result.returncode, result.stdout, result.stderr)
    assert result.returncode == 0 and events[-1]["event"] == "guest_exit" and events[-1]["exit_code"] == 0, \
        ("a nonzero exit code is the source line of the first failed guest check",
         result.returncode, events[-3:], result.stderr[-2000:])
    assert not [event for event in events if event["event"] in ("unresolved_import", "missing_module")], events
    print("PASS 96 leaf imports resolve under --strict-imports; bridged upstream exports and native "
          "stand-ins return the upstream status codes and bytes")
