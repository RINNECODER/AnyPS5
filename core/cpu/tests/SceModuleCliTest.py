import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


runner, main_fixture, module_fixture, linux_fixture = map(lambda value: Path(value).resolve(), sys.argv[1:])


def run(arguments, cwd):
    result = subprocess.run([str(runner), "--diagnostics-json", *map(str, arguments)],
                            cwd=cwd, capture_output=True, text=True, timeout=20)
    assert result.stdout == "", (result.returncode, result.stdout, result.stderr)
    events = [json.loads(line) for line in result.stderr.splitlines()]
    assert events and all(event["schema_version"] == 1 for event in events), events
    return result, events


def rejected(arguments, cwd, code, message):
    result, events = run(arguments, cwd)
    assert result.returncode == 126, (result.returncode, result.stderr)
    assert len(events) == 1 and events[0]["event"] == "error", events
    assert events[0]["code"] == code and events[0]["process_exit"] == 126, events
    assert message in events[0]["message"], events


with tempfile.TemporaryDirectory(prefix="anyps5 module CLI é ") as directory:
    root = Path(directory)
    images = root / "compiled images with spaces"
    other_cwd = root / "unrelated working directory"
    images.mkdir()
    other_cwd.mkdir()
    main = images / main_fixture.name
    module = images / "SceModuleGuest.prx"
    shutil.copyfile(main_fixture, main)
    shutil.copyfile(module_fixture, module)
    module_argument = os.path.relpath(module, other_cwd)
    for checksum, status in ((3366582378, 0), (3366582379, 77)):
        result, events = run(["--sce-module", module_argument, main, 17, 5, 7, 58, checksum], other_cwd)
        assert result.returncode == status, (status, result.returncode, result.stderr)
        assert [event["event"] for event in events] == ["startup", "guest_exit"], events
        started, exited = events
        assert started["executable"] == str(main) and started["entry"] > 0, started
        assert started["host_architecture"] == "arm64" and started["guest_architecture"] == "x86_64", started
        assert started["format"] == "sce_elf64_x86_64", started
        assert exited["exit_code"] == status, exited
    print("compiled SCE module CLI: relative dependency from other cwd, spaces, initialized objects/TLS, prime/Adler result and guest exits0/77 PASS")

    rejected([main, 17, 5, 7, 58, 3366582378], other_cwd,
             "unsupported_executable", "DT_NEEDED guest module loading is unsupported")
    missing_module = images / "missing dependency.prx"
    rejected(["--sce-module", missing_module, main, 17, 5, 7, 58, 3366582378], other_cwd,
             "input_unavailable", str(missing_module))
    print("declared dependency omitted or unavailable: explicit failures before startup/guest entry PASS")

    for arguments in (["--sce-module", module_argument, linux_fixture],
                      ["--inspect-sce-json", "--sce-module", module_argument, main],
                      ["--sce-module"]):
        rejected(arguments, other_cwd, "invalid_arguments", "--sce-module")
    print("module option on static Linux/inspection and missing module argument: explicit CLI rejection PASS")
