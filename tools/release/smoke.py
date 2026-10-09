#!/usr/bin/env python3
"""Local title smoke for beta promotion: how far does the engine get booting the test title?

    python3 tools/release/smoke.py --package <accepted package dir> [--title-dir DIR]

The title dump is never committed: its directory comes from ``--title-dir`` or the
runner environment variable ANYPS5_E2E_TITLE_DIR. The directory holds ``eboot.bin`` plus
the ``.prx`` modules MacPS passes with ``--sce-module`` (``libc.prx`` first, then the rest
sorted), and is also the resource root, exactly as MacPS launches an unmounted title.

The title does not boot yet, so the smoke records the furthest boot stage reached from
the engine's ``--diagnostics-json`` events instead of requiring a pass:

  0 no_diagnostics          crashed, hung or printed no diagnostics event
  1 launch_rejected         invalid arguments, unreadable input or host failure
  2 load_rejected           the loader or module graph refused the title (e.g. missing import)
  3 dependency_init_failed  modules loaded; a dependency initializer failed in guest code
  4 entered_main            the startup event: the title's entry point was reached
  5 guest_exit              the title ran to a guest exit

Beta promotion requires candidate stage >= current beta stage. Output never contains the
title's paths or hashes (workflow logs are public): paths are redacted from messages.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

STAGES = ("no_diagnostics", "launch_rejected", "load_rejected", "dependency_init_failed", "entered_main", "guest_exit")
LAUNCH_CODES = {"invalid_arguments", "input_unavailable", "host_failure"}
LOAD_CODES = {"unsupported_executable", "loader_failure"}
EXECUTION_CODES = {"unsupported_instruction", "unsupported_service", "guest_memory_fault",
                   "execution_limit", "execution_failure"}


def classify(stderr_text):
    """Return (stage index, last error code or None, last error message or None)."""
    events = []
    for line in stderr_text.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if isinstance(event, dict) and event.get("schema_version") == 1 and isinstance(event.get("event"), str):
            events.append(event)
    kinds = [event["event"] for event in events]
    errors = [event for event in events if event["event"] == "error"]
    code = errors[-1].get("code") if errors else None
    message = errors[-1].get("message") if errors else None
    if "guest_exit" in kinds:
        return 5, code, message
    if "startup" in kinds:
        return 4, code, message
    if code in EXECUTION_CODES:
        return 3, code, message
    if code in LOAD_CODES:
        return 2, code, message
    if code in LAUNCH_CODES:
        return 1, code, message
    return 0, code, message


def redact(text, title_dir):
    if not text:
        return text
    for secret, replacement in ((str(Path(title_dir).resolve()), "<title>"), (str(title_dir), "<title>"),
                                (str(Path.home()), "~")):
        text = text.replace(secret, replacement)
    return text[:400]


def title_arguments(title_dir):
    title_dir = Path(title_dir)
    modules = sorted(title_dir.glob("*.prx"), key=lambda p: (p.name != "libc.prx", p.name))
    args = ["--resource-root", str(title_dir), "--diagnostics-json"]
    for module in modules:
        args += ["--sce-module", str(module)]
    return args + [str(title_dir / "eboot.bin")]


def run_smoke(package, title_dir, timeout=180):
    package, title_dir = Path(package), Path(title_dir)
    engine = package / "bin" / "anyps5_cpu_run"
    if not engine.is_file() or not (title_dir / "eboot.bin").is_file():
        raise RuntimeError("smoke needs an accepted package and a title directory with eboot.bin")
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "LD_"))}
    started = time.monotonic()
    timed_out = False
    process = subprocess.Popen([str(engine), *title_arguments(title_dir)], cwd=str(title_dir), env=env,
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               start_new_session=True)
    try:
        _, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        os.killpg(process.pid, 9)
        _, stderr = process.communicate()
    stage, code, message = classify(stderr.decode(errors="replace"))
    return {"stage": stage, "stage_name": STAGES[stage], "error_code": code, "message": redact(message, title_dir),
            "exit_status": process.returncode, "timed_out": timed_out, "seconds": round(time.monotonic() - started, 1)}


def with_title_lease(function):
    """Hold the shared title-session lease so the smoke never overlaps a MacPS or agent title run."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from prepare_diagnostic_engine import Lease  # noqa: E402  (tools/ is not a package)
    with Lease(["title-session"], owner="release-smoke"):
        return function()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--title-dir", type=Path, default=os.environ.get("ANYPS5_E2E_TITLE_DIR"))
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args(argv)
    if not args.title_dir:
        print("ANYPS5_E2E_TITLE_DIR is not set; the smoke cannot run", file=sys.stderr)
        return 2
    result = with_title_lease(lambda: run_smoke(args.package, args.title_dir, args.timeout))
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
