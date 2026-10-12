#!/usr/bin/env python3
"""Run a title directly under Rosetta 2 (docs/design/rosetta-direct-execution.md) for development.

usage: run_title.py <title-dir> [--resource-root DIR] [--build DIR | --launcher PATH]
                    [--cache DIR] [--wall SECONDS] [--log FILE] [--relink] [--trace REGEX] [-- guest-args...]

A thin wrapper over the engine launcher, anyps5_rosetta_run (tools/rosetta/launcher; the
engine package's executable, built with the Rosetta libs into <build>/rosetta/bin). The
launcher unselfs and relinks the title (cached by the eboot's SHA-256 under
~/Library/Caches/AnyPS5/rosetta/<sha>/), lays out libs/ and app0/, stubs imports no
x86_64 library exports, and runs it. This script adds the wall limit, a log file and a
summary: exit or signal, seconds, guest entry time, the fault report, the first
unimplemented NIDs and the last ones called.

Guest modules: <title>/sce_module/*.prx, <resource>/sce_module/*.prx and, for a launcher
title directory, <title>/*.prx are preloaded; the resource root's top-level *.prx are
relinked as deferred modules for sceKernelLoadStartModule. Library functions that exist
but are not implemented log once and return 0 (ANYPS5_UNIMPLEMENTED=continue) unless
--abort-on-unimplemented is passed.
"""
import argparse
import hashlib
import json
import os
import signal
import subprocess
import sys
from pathlib import Path


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def summarize(status, timed_out, log_path, layout):
    lines = log_path.read_text(errors="replace").splitlines()
    first = lambda key: next((line for line in lines if key in line), None)
    stub_calls = [line for line in lines if "[rosetta] unimplemented import" in line]
    exits = [json.loads(line) for line in lines if line.startswith('{"schema_version":1,"event":"guest_exit"')]
    return {
        "exit": status if status < 128 else None,
        "signal": signal.Signals(status - 128).name if 128 < status < 128 + 32 else None,
        "timed_out": timed_out,
        "seconds": exits[-1]["seconds"] if exits else None,
        "guest_entry": first("[rosetta] guest entry "),
        "guest_returned": first("[rosetta] guest entry returned"),
        "fault": first("[rosetta] guest fault"),
        "uncaught_exception": first("terminating due to uncaught exception") or first("libc++abi: terminating"),
        "dyld_error": first("dyld["),
        "launcher_error": first("[anyps5] error:"),
        "first_unimplemented": stub_calls[0].split("import ", 1)[1] if stub_calls else None,
        "last_imports_called": [line.split("import ", 1)[1] for line in stub_calls[-5:]],
        "distinct_unimplemented_called": len(stub_calls),
        "missing_libs": layout.get("missing_libs"),
        "unresolved_imports": layout.get("unresolved"),
        "log": str(log_path),
        "log_tail": lines[-12:],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("title", type=Path)
    parser.add_argument("--resource-root", type=Path)
    parser.add_argument("--build", type=Path, help="x86_64 build dir configured with -DANYPS5_ROSETTA_LIBS=ON")
    parser.add_argument("--launcher", type=Path, help="anyps5_rosetta_run (default <build>/rosetta/bin/anyps5_rosetta_run)")
    parser.add_argument("--cache", type=Path, help="cache root (default ~/Library/Caches/AnyPS5/rosetta)")
    parser.add_argument("--wall", type=float, default=600)
    parser.add_argument("--log", help="run log (default <cache>/<sha>.run.log)")
    parser.add_argument("--relink", action="store_true")
    parser.add_argument("--abort-on-unimplemented", action="store_true",
                        help="let library functions that are not implemented throw (default: log once and return 0)")
    parser.add_argument("--trace", metavar="REGEX",
                        help="log guest calls to imports whose NID or name matches REGEX (ANYPS5_TRACE_LIMIT calls each, default 16; "
                             "ANYPS5_TRACE_AFTER=SECONDS skips calls made before then; ANYPS5_TRACE_RETURNS=1 also logs each logged call's %%rax)")
    parser.add_argument("guest_args", nargs="*")
    args = parser.parse_args()
    launcher = args.launcher or (args.build / "rosetta" / "bin" / "anyps5_rosetta_run" if args.build else None)
    if launcher is None or not launcher.is_file():
        parser.error("pass --build (with the rosetta_engine target built) or --launcher")
    title = args.title.resolve()
    resource = (args.resource_root or args.title).resolve()
    eboot = title / "eboot.bin"
    if not eboot.exists():
        eboot = resource / "eboot.bin"
    if not eboot.exists():
        raise SystemExit("no eboot.bin in the title directory or resource root")
    cache_root = (args.cache or Path.home() / "Library" / "Caches" / "AnyPS5" / "rosetta").expanduser()
    cache = cache_root / sha256(eboot)
    # Beside the cache directory: a relink replaces <sha>/ while the title is being prepared.
    log_path = Path(args.log or cache_root / (cache.name + ".run.log")).resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    command = [str(launcher), "--cache", str(cache_root), "--resource-root", str(resource), "--diagnostics-json", "--wall", str(args.wall)]
    command += ["--relink"] if args.relink else []
    command += ["--abort-on-unimplemented"] if args.abort_on_unimplemented else []
    command += ["--trace", args.trace] if args.trace else []
    command += [str(eboot), "--", *args.guest_args]
    print(f"[run_title] {' '.join(command[:1])} ... running for up to {args.wall:.0f}s; log {log_path}", flush=True)
    with open(log_path, "wb") as stream:
        status = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT).returncode
    lines = log_path.read_text(errors="replace").splitlines()
    timed_out = any('"timed_out":true' in line for line in lines if '"guest_exit"' in line)
    layout_path = cache / "layout.json"
    layout = json.loads(layout_path.read_text()) if layout_path.is_file() else {}
    summary = summarize(status, timed_out, log_path, layout)
    if cache.is_dir():
        (cache / "summary.json").write_text(json.dumps(summary, indent=1))
    for key, value in summary.items():
        if key != "log_tail":
            print(f"{key}: {value}")
    print("log tail:")
    for line in summary["log_tail"]:
        print(f"  {line[:240]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
