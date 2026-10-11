"""A guest calling the NP family gets upstream core/libs/prx results through the bridge.

The guest (fixtures/UpstreamNpBridgeHomebrew.c) asserts upstream's return codes,
handles and output bytes and exits with a distinct status per failed check.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from CliDiagnostics import parse_diagnostics


runner, main_fixture, guest_fixture = (Path(value).resolve() for value in sys.argv[1:4])


def run(cwd, main, module, owned, entitlements=None):
    environment = {key: value for key, value in os.environ.items() if key != "ANYPS5_ENTITLEMENTS"}
    if entitlements is not None:
        environment["ANYPS5_ENTITLEMENTS"] = str(entitlements)
    result = subprocess.run(
        [str(runner), "--diagnostics-json", "--strict-imports", "--sce-module", module, str(main), owned],
        cwd=cwd, env=environment, capture_output=True, text=True, timeout=30)
    assert result.stdout == "", (result.returncode, result.stdout, result.stderr)
    return result, parse_diagnostics(result.stderr)


def exited(result, events, status=0):
    assert result.returncode == status and events[-1]["event"] == "guest_exit" and \
        events[-1]["exit_code"] == status, (status, result.returncode, events, result.stderr)


with tempfile.TemporaryDirectory(prefix="anyps5 np bridge ") as directory:
    root = Path(directory)
    images, cwd = root / "guest images", root / "unrelated cwd"
    images.mkdir()
    cwd.mkdir()
    main = images / main_fixture.name
    shutil.copyfile(main_fixture, main)
    shutil.copyfile(guest_fixture, images / guest_fixture.name)
    module = os.path.relpath(images / guest_fixture.name, cwd)

    # Strict imports: every NP import resolves and every declared NP module is provided,
    # so the load cannot fall back to trap stubs.
    result, events = run(cwd, main, module, "0")
    exited(result, events)
    assert not [event for event in events if event["event"] in ("unresolved_import", "missing_module")], events
    print("PASS upstream NpManager/NpWebApi2/NpAuth/NpEntitlementAccess results reach the guest "
          "under --strict-imports; no entitlements without a configured list")

    owned = root / "entitlements é.ini"
    owned.write_text("# owned add-ons\nANYPS5DLCA\n  ANYPS5DLCB ; trailing comment\n\n")
    result, events = run(cwd, main, module, "2", owned)
    exited(result, events)
    print("PASS ANYPS5_ENTITLEMENTS list reaches the guest: bounded list copy, label lookup, info output")

    # Upstream treats a configured but unreadable list as a fatal configuration error;
    # the bridge stops the guest with that message instead of crashing the host.
    result, events = run(cwd, main, module, "0", root / "missing.ini")
    assert result.returncode != 0 and events[-1]["event"] == "error", (result.returncode, events, result.stderr)
    message = events[-1]["message"]
    for part in ("sceNpEntitlementAccessGetAddcontEntitlementInfoList", "cannot read", "missing.ini"):
        assert part in message, (part, message)
    print("PASS unreadable ANYPS5_ENTITLEMENTS stops the guest with upstream's error")
