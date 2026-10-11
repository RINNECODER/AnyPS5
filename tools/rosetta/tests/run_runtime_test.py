#!/usr/bin/env python3
"""Build, relink and run the Rosetta runtime fixture (guest TLS, module start).

usage: run_runtime_test.py <relinker> <rosetta-libs-dir> <work-dir> {threads|fault}

threads: the fixture checks per-thread TLS in every image (initial,
         startup-dependency and dlopen'd modules) across scePthreadCreate
         threads, module_start arguments/result, sceKernelDlsym and
         sceKernelGetModuleInfoFromAddr handles; exit 0 is a pass.
fault:   the fixture stores to address 2; the runtime must exit with
         128+signal and report the fault address and the guest RIP.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures"
CFLAGS = ["--target=x86_64-unknown-linux-gnu", "-O2", "-fPIC", "-ffreestanding", "-fno-builtin",
          "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
          # %fs:0 plus an offset, the form the relinker's macOS TLS stubs accept.
          "-mno-tls-direct-seg-refs"]
LDFLAGS = ["-shared", "-Bsymbolic", "--hash-style=sysv", "-z", "max-page-size=4096", "-z", "separate-code", "-z", "norelro"]
KERNEL_IMPORTS = ["scePthreadCreate", "scePthreadJoin", "sceKernelLoadStartModule", "sceKernelDlsym",
                  "sceKernelGetModuleInfoFromAddr"]


def run(*command, **kwargs):
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def linker():
    for candidate in (shutil.which("ld.lld"), "/opt/homebrew/opt/lld/bin/ld.lld", "/opt/homebrew/opt/llvm/bin/ld.lld"):
        if candidate and Path(candidate).exists():
            return candidate
    raise SystemExit("ld.lld not found")


def build(relinker, libs, work):
    if work.exists():
        shutil.rmtree(work)
    objects, src, out = work / "obj", work / "src", work / "out"
    for directory in (objects, src / "sce_module", src / "dynamic", out):
        directory.mkdir(parents=True)
    lld = linker()
    for name in ("GuestModule", "GuestDynamic", "GuestMain"):
        run("clang", *CFLAGS, "-c", FIXTURES / f"{name}.c", "-o", objects / f"{name}.o")
    run("clang", "--target=x86_64-unknown-linux-gnu", "-c", FIXTURES / "GuestStart.S", "-o", objects / "GuestStart.o")
    run(lld, *LDFLAGS, "-soname", "GuestModule.prx", "-init", "ModuleInit", "-o", objects / "GuestModule.elf", objects / "GuestModule.o")
    run(lld, *LDFLAGS, "-soname", "GuestDynamic.prx", "-init", "DynamicStart", "-o", objects / "GuestDynamic.elf", objects / "GuestDynamic.o")
    # The main image links like an executable (local-exec TLS resolved at link
    # time, as in retail eboots); --no-relax keeps imported TLS general-dynamic.
    run(lld, *LDFLAGS[1:], "-pie", "--no-relax", "-z", "undefs", "--allow-shlib-undefined", "--export-dynamic", "-e", "_start", "--no-as-needed", "-o", objects / "GuestMain.elf",
        objects / "GuestStart.o", objects / "GuestMain.o", objects / "GuestModule.elf")
    package = [sys.executable, HERE / "package_fixture.py"]
    run(*package, objects / "GuestModule.elf", src / "sce_module" / "GuestModule.prx", "--module", "GuestModule",
        "--import-lib", "libkernel:__tls_get_addr")
    # Not under sce_module: --module-dir modules are relinked but not preloaded.
    run(*package, objects / "GuestDynamic.elf", src / "dynamic" / "GuestDynamic.prx", "--module", "GuestDynamic")
    run(*package, objects / "GuestMain.elf", src / "eboot.elf", "--module", "GuestMain", "--main",
        "--import-lib", "GuestModule:ModuleInitCount=obj,ModuleInitArgs=obj,ModuleSwapZero,ModuleTlsGet,ModuleTlsSet",
        "--import-lib", "libkernel:" + ",".join(KERNEL_IMPORTS))
    with open(work / "relink.log", "w") as log:
        result = subprocess.run([str(relinker), "--macos", "--to-rosetta", "--module-dir", "dynamic", str(src / "eboot.elf"), str(out / "eboot")],
                                stdout=log, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        print((work / "relink.log").read_text()[-3000:])
        raise SystemExit(f"relinker failed ({result.returncode})")
    os.chmod(out / "eboot", 0o755)
    (out / "libs").symlink_to(Path(libs).resolve(), target_is_directory=True)
    return out


def main():
    if len(sys.argv) != 5 or sys.argv[4] not in ("threads", "fault"):
        raise SystemExit(__doc__)
    relinker, libs, work, mode = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]) / sys.argv[4], sys.argv[4]
    out = build(relinker, libs, work)
    command = ["arch", "-x86_64", "./eboot"] + (["fault"] if mode == "fault" else [])
    result = subprocess.run(command, cwd=out, capture_output=True, text=True, timeout=120)
    print(result.stderr[-4000:], end="")
    print(f"exit status {result.returncode}")
    if mode == "threads":
        if result.returncode != 0:
            raise SystemExit(f"FAIL: fixture check {result.returncode} failed (see GuestMain.c)")
        if "[rosetta] guest entry" not in result.stderr:
            raise SystemExit("FAIL: Aps5StartGuest did not report the guest entry")
    else:
        if result.returncode not in (128 + 10, 128 + 11):
            raise SystemExit(f"FAIL: expected exit 138/139 from the fault report, got {result.returncode}")
        fault = next((line for line in result.stderr.splitlines() if "guest fault: SIG" in line), "")
        if "addr=0x2 " not in fault or "eboot+" not in fault or " guest=0x" not in fault:
            raise SystemExit("FAIL: fault report lacks the fault address, eboot offset or guest RIP")
    print("PASS")


if __name__ == "__main__":
    main()
