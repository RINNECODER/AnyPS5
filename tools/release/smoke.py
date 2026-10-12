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

The engine also writes frames of its own game window (ANYPS5_FRAME_CAPTURE_DIR, set to
<scratch>/frames), and a window-only screenshot of the engine's window is taken at the same
interval (tools/release/window_capture.py). The summary adds frame fields and ``boot_visible``:
true when a captured frame has nonblack_ratio > 0.05 and distinct_colors >= 8. Captured
images stay in the scratch directory and are never committed.

Beta promotion requires candidate stage >= current beta stage. Output never contains the
title's paths or hashes (workflow logs are public): paths are redacted from messages.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
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


def frame_summary(stderr_text):
    """Frame diagnostics from the engine: presented count, last capture and the BOOT-VISIBLE verdict."""
    presented = 0
    captures = []
    for line in stderr_text.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if not isinstance(event, dict):
            continue
        if event.get("event") == "frames_presented" and isinstance(event.get("count"), int):
            presented = max(presented, event["count"])
        elif event.get("event") == "frame_captured":
            captures.append(event)
            presented = max(presented, int(event.get("flip_index", 0)) + 1)
    last = captures[-1] if captures else None
    visible = any(c.get("nonblack_ratio", 0) > 0.05 and c.get("distinct_colors", 0) >= 8 for c in captures)
    return {"frames_presented": presented, "captures": len(captures),
            "last_capture_ms": last.get("ms") if last else None,
            "last_nonblack_ratio": last.get("nonblack_ratio") if last else None,
            "last_distinct_colors": last.get("distinct_colors") if last else None,
            "boot_visible": visible}


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


def run_smoke(package, title_dir, timeout=180, scratch=None, frame_interval=30):
    package, title_dir = Path(package), Path(title_dir)
    engine = package / "bin" / "anyps5_cpu_run"
    if not engine.is_file() or not (title_dir / "eboot.bin").is_file():
        raise RuntimeError("smoke needs an accepted package and a title directory with eboot.bin")
    scratch = Path(scratch) if scratch else Path(tempfile.mkdtemp(prefix="anyps5-smoke-"))
    frames, shots = scratch / "frames", scratch / "windows"
    shots.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "LD_"))}
    env["ANYPS5_FRAME_CAPTURE_DIR"] = str(frames)
    env["ANYPS5_FRAME_CAPTURE_INTERVAL_MS"] = str(int(frame_interval * 1000))
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import window_capture  # noqa: E402
    started = time.monotonic()
    timed_out = False
    process = subprocess.Popen([str(engine), *title_arguments(title_dir)], cwd=str(title_dir), env=env,
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               start_new_session=True)
    window_status = None
    stderr = b""
    while True:
        remaining = timeout - (time.monotonic() - started)
        try:
            _, stderr = process.communicate(timeout=max(0.1, min(frame_interval, remaining)))
            break
        except subprocess.TimeoutExpired:
            if remaining <= frame_interval:
                timed_out = True
                os.killpg(process.pid, 9)
                _, stderr = process.communicate()
                break
            # Window-only: the engine's own window, selected by its PID. Never the display.
            name = "window-%d.png" % int((time.monotonic() - started) * 1000)
            window_status = window_capture.capture(process.pid, shots / name)
    text = stderr.decode(errors="replace")
    stage, code, message = classify(text)
    return {"stage": stage, "stage_name": STAGES[stage], "error_code": code, "message": redact(message, title_dir),
            "exit_status": process.returncode, "timed_out": timed_out, "seconds": round(time.monotonic() - started, 1),
            **frame_summary(text), "window_screenshots": len(list(shots.glob("*.png"))),
            "window_capture_status": window_status["status"] if window_status else None,
            "frames_dir": str(frames)}


def with_title_lease(function):
    """Run the smoke; the shared title-session lease went away with the TCG packaging tools."""
    return function()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--title-dir", type=Path, default=os.environ.get("ANYPS5_E2E_TITLE_DIR"))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--scratch", type=Path, default=None, help="scratch dir for frames/ and windows/ (default: new temp dir)")
    parser.add_argument("--frame-interval", type=float, default=30, help="seconds between frame and window captures")
    args = parser.parse_args(argv)
    if not args.title_dir:
        print("ANYPS5_E2E_TITLE_DIR is not set; the smoke cannot run", file=sys.stderr)
        return 2
    result = with_title_lease(lambda: run_smoke(args.package, args.title_dir, args.timeout, args.scratch, args.frame_interval))
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
