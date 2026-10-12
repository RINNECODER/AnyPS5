#!/usr/bin/env python3
"""Run a synthetic title through the engine launcher the way MacPS launches one.

usage: launcher_smoke.py <engine root> <work dir>

<engine root> is an unpacked engine package (bin/anyps5_rosetta_run, bin/relinker,
libs/, share/) or a Rosetta build's rosetta/ directory, which has the same layout.
The title (fixtures/GuestSmoke.c) is built with clang + ld.lld into <work>/title
(eboot.bin plus GuestModule.prx, passed with --sce-module) and <work>/resource (the
resource root: GuestDynamic.prx, loaded at run time, and data/smoke.txt). Checks:

* cold run: a relink 'preparing' event, the startup event, guest_exit with exit 0,
  and the stub log for the import no library exports and the missing library;
* warm run: the relink is reused (no relink stage) and the title exits 0 again;
* a missing executable fails with an input_unavailable error event and a non-zero exit.

Prints one JSON summary line (cold/warm seconds) and PASS. Never uses a retail title.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import package_fixture  # noqa: E402
from run_runtime_test import CFLAGS, FIXTURES, LDFLAGS, linker, run  # noqa: E402

TITLE_IMPORTS = ["--import-lib", "GuestModule:ModuleInitCount=obj",
                 "--import-lib", "libkernel:sceKernelLoadStartModule,sceKernelDlsym,sceKernelOpen,sceKernelRead,"
                                 "sceKernelClose,Aps5SmokeUnimplemented",
                 "--import-lib", "libSceAps5Smoke:Aps5SmokeMissingLibrary"]


def build_title(work):
    objects, title, resource = work / "obj", work / "title", work / "resource"
    for directory in (objects, title, resource / "data"):
        directory.mkdir(parents=True)
    lld = linker()
    for name in ("GuestModule", "GuestDynamic", "GuestSmoke"):
        run("clang", *CFLAGS, "-c", FIXTURES / f"{name}.c", "-o", objects / f"{name}.o")
    run("clang", "--target=x86_64-unknown-linux-gnu", "-c", FIXTURES / "GuestStart.S", "-o", objects / "GuestStart.o")
    run(lld, *LDFLAGS, "-soname", "GuestModule.prx", "-init", "ModuleInit", "-o", objects / "GuestModule.elf", objects / "GuestModule.o")
    run(lld, *LDFLAGS, "-soname", "GuestDynamic.prx", "-init", "DynamicStart", "-o", objects / "GuestDynamic.elf", objects / "GuestDynamic.o")
    run(lld, *LDFLAGS[1:], "-pie", "--no-relax", "-z", "undefs", "--allow-shlib-undefined", "--export-dynamic", "-e", "_start",
        "--no-as-needed", "-o", objects / "GuestSmoke.elf", objects / "GuestStart.o", objects / "GuestSmoke.o", objects / "GuestModule.elf")
    package = [sys.executable, HERE / "package_fixture.py"]
    run(*package, objects / "GuestModule.elf", title / "GuestModule.prx", "--module", "GuestModule",
        "--import-lib", "libkernel:__tls_get_addr")
    run(*package, objects / "GuestDynamic.elf", resource / "GuestDynamic.prx", "--module", "GuestDynamic")
    run(*package, objects / "GuestSmoke.elf", title / "eboot.bin", "--module", "GuestSmoke", "--main", *TITLE_IMPORTS)
    (resource / "data" / "smoke.txt").write_text("smoke")
    return title, resource


def launch(engine, title, resource, cache, eboot=None):
    """MacPS's invocation: --resource-root, --diagnostics-json, --sce-module..., executable."""
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "LD_"))}
    env["ANYPS5_ROSETTA_CACHE"] = str(cache)
    command = [str(engine), "--resource-root", str(resource), "--diagnostics-json",
               "--sce-module", str(title / "GuestModule.prx"), str(eboot or title / "eboot.bin")]
    started = time.monotonic()
    result = subprocess.run(command, cwd=str(title), env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=180)
    events = []
    for line in (result.stdout + "\n" + result.stderr).splitlines():
        if line.startswith("{"):
            try:
                event = json.loads(line)
            except ValueError:
                continue
            if isinstance(event, dict) and event.get("schema_version") == 1:
                events.append(event)
    return result, events, time.monotonic() - started


def require(condition, message, result=None):
    if not condition:
        if result is not None:
            print(result.stderr[-3000:])
        raise SystemExit("FAIL: " + message)


def check_run(name, result, events, relinked):
    kinds = [event["event"] for event in events]
    stages = {event.get("stage") for event in events if event["event"] == "preparing"}
    exits = [event for event in events if event["event"] == "guest_exit"]
    require(result.returncode == 0, f"{name}: launcher exit {result.returncode}", result)
    require("startup" in kinds, f"{name}: no startup event", result)
    require(exits and exits[-1].get("exit_code") == 0, f"{name}: guest_exit {exits[-1:]}", result)
    require(("relink" in stages or "unself" in stages) == relinked,
            f"{name}: expected relink={relinked}, preparing stages {sorted(s for s in stages if s)}", result)
    require("ready" in stages, f"{name}: no ready stage", result)
    for symbol in ("Aps5SmokeUnimplemented", "Aps5SmokeMissingLibrary"):
        require(f"unimplemented import {package_fixture.nid(symbol)}" in result.stderr, f"{name}: no stub log for {symbol}", result)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    root, work = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    engine = root / "bin" / "anyps5_rosetta_run"
    require(engine.is_file(), f"no launcher at {engine}")
    if work.exists():
        shutil.rmtree(work)
    title, resource = build_title(work)
    cache = work / "cache"

    result, events, cold = launch(engine, title, resource, cache)
    check_run("cold", result, events, relinked=True)
    result, events, warm = launch(engine, title, resource, cache)
    check_run("warm", result, events, relinked=False)

    result, events, _ = launch(engine, title, resource, cache, eboot=title / "missing.bin")
    errors = [event for event in events if event["event"] == "error"]
    require(result.returncode != 0 and errors and errors[-1].get("code") == "input_unavailable",
            f"missing executable: exit {result.returncode}, errors {errors}", result)

    print(json.dumps({"cold_seconds": round(cold, 2), "warm_seconds": round(warm, 2)}))
    print("PASS")


if __name__ == "__main__":
    main()
