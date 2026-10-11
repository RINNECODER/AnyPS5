#!/usr/bin/env python3
"""Window-only screenshot of one process's on-screen game window (smoke tooling only).

    python3 tools/release/window_capture.py --pid <pid> --out <file.png> [--find-only]

Finds the process's layer-0 on-screen window through Quartz (kCGWindowOwnerPID == pid) and
runs ``screencapture -x -o -l <windowID> <file>``: that window only, no shadow, no sound.
screencapture is never called without ``-l``, so the full display and other apps' windows
are never captured. With no window, or without Screen Recording permission, it reports that
and exits 0: the engine-side capture (ANYPS5_FRAME_CAPTURE_DIR) stays the source of truth.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

_JXA = """
ObjC.import('CoreGraphics');
const raw = $.CGWindowListCopyWindowInfo($.kCGWindowListOptionOnScreenOnly, $.kCGNullWindowID);
const list = ObjC.deepUnwrap(ObjC.castRefToObject(raw));
JSON.stringify(list.map(w => ({pid: w.kCGWindowOwnerPID, layer: w.kCGWindowLayer,
    id: w.kCGWindowNumber, bounds: w.kCGWindowBounds})));
"""


def _window_list():
    try:
        import Quartz  # PyObjC
        rows = Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID) or []
        return [{"pid": row.get("kCGWindowOwnerPID"), "layer": row.get("kCGWindowLayer"),
                 "id": row.get("kCGWindowNumber"), "bounds": dict(row.get("kCGWindowBounds") or {})} for row in rows]
    except ImportError:
        pass
    result = subprocess.run(["osascript", "-l", "JavaScript", "-e", _JXA], capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        raise RuntimeError("cannot list windows: " + result.stderr.strip()[:200])
    return json.loads(result.stdout)


def find_window(pid):
    """Largest layer-0 on-screen window owned by ``pid``, or None."""
    candidates = [row for row in _window_list() if row.get("pid") == pid and row.get("layer") == 0]

    def area(row):
        bounds = row.get("bounds") or {}
        return (bounds.get("Width") or 0) * (bounds.get("Height") or 0)

    return max(candidates, key=area) if candidates else None


def capture(pid, out, find_only=False):
    """Returns a status dict; never raises for the expected no-window / no-permission cases."""
    try:
        window = find_window(pid)
    except (RuntimeError, OSError, subprocess.SubprocessError, ValueError) as error:
        return {"status": "no_window_list", "detail": str(error)[:200]}
    if window is None:
        return {"status": "no_window", "pid": pid}
    if find_only:
        return {"status": "found", "window_id": window["id"]}
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    # -l <id> is mandatory: it restricts the capture to that single window.
    result = subprocess.run(["screencapture", "-x", "-o", "-l", str(window["id"]), str(out)],
                            capture_output=True, text=True, timeout=30)
    if result.returncode != 0 or not out.is_file() or out.stat().st_size == 0:
        return {"status": "screencapture_failed", "detail": (result.stderr or "").strip()[:200],
                "hint": "Screen Recording permission may be missing for the terminal"}
    return {"status": "captured", "window_id": window["id"], "path": str(out)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--out", type=Path, default=Path("window.png"))
    parser.add_argument("--find-only", action="store_true", help="locate the window without capturing")
    args = parser.parse_args(argv)
    print(json.dumps(capture(args.pid, args.out, args.find_only)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
