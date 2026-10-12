#!/usr/bin/env python3
"""Run a title directly under Rosetta 2 (docs/design/rosetta-direct-execution.md).

usage: run_title.py <title-dir> [--resource-root DIR] [--build DIR | --libs DIR --relinker PATH]
                    [--cache DIR] [--wall SECONDS] [--log FILE] [--relink] [-- guest-args...]

1. unself eboot.bin and the guest modules (tools/unself.py);
2. relink with `relinker --macos --to-rosetta`, cached by the eboot's SHA-256
   under ~/Library/Caches/AnyPS5/rosetta/<sha>/ (reused while the inputs and
   the relinker are unchanged; --relink forces it);
3. lay out <cache>/out: eboot, libs/ (the x86_64 libs/*.prx plus generated
   stubs for libraries the build excludes), app0/ (the resource root linked in
   read-only, with the relinked sce_module/*.guest.prx beside the originals);
4. run `arch -x86_64 ./eboot` there with a wall limit, logging to a file;
5. print a summary: exit or signal, seconds, guest entry time, the fault
   report, the first unimplemented NIDs and the last ones called.

Guest modules: <title>/sce_module/*.prx, <resource>/sce_module/*.prx and, for a
launcher title directory, <title>/*.prx are preloaded like sce_module; the
resource root's top-level *.prx are relinked as deferred modules for
sceKernelLoadStartModule. Imports that no x86_64 library exports are bound to
logging stubs (DYLD_INSERT_LIBRARIES) that return 0. Library functions that
exist but are not implemented log once and return 0 as well
(ANYPS5_UNIMPLEMENTED=continue) unless --abort-on-unimplemented is passed.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UNSELF = ROOT / "tools" / "unself.py"
STUB_RUNTIME = Path(__file__).resolve().parent / "stub_runtime.c"
HOOKS = {"_Aps5StartGuest_nid_no_patch", "_Aps5GuestTlsAllocate_nid_no_patch", "_Aps5GuestTlsRegister_nid_no_patch",
         "_Aps5GuestTlsGetAddr_nid_no_patch", "___aps5_get_pending_module_args_nid_no_patch",
         "___aps5_set_module_init_result_nid_no_patch"}
NID_DB = Path(tempfile.gettempdir()) / "anyps5-nid-db" / "aerolib.csv"
NID_SYMBOL = re.compile(r"^_[A-Za-z0-9+-]{11}(#|$)")


def log(message):
    print(f"[run_title] {message}", flush=True)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def uleb(data, i):
    value = shift = 0
    while True:
        byte = data[i]
        i += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return value, i


def macho_binds(path):
    """Imported symbol names and @rpath libraries of a Mach-O image (classic bind opcodes)."""
    data = path.read_bytes()
    magic, _, _, _, ncmds = struct.unpack_from("<IiiII", data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit(f"not an x86_64 Mach-O image: {path}")
    offset, binds, dylibs = 32, set(), []
    for _ in range(ncmds):
        command, size = struct.unpack_from("<II", data, offset)
        if command in (0x22, 0x80000022):
            for start, length in (struct.unpack_from("<II", data, offset + 16), struct.unpack_from("<II", data, offset + 32),
                                  struct.unpack_from("<II", data, offset + 24)):
                i, end = start, start + length
                while i < end:
                    opcode = data[i] & 0xF0
                    i += 1
                    if opcode == 0x40:
                        j = data.index(b"\0", i)
                        binds.add(data[i:j].decode())
                        i = j + 1
                    elif opcode in (0x10, 0x60, 0x70, 0xA0):
                        _, i = uleb(data, i)
                    elif opcode == 0xC0:
                        _, i = uleb(data, i)
                        _, i = uleb(data, i)
        elif command in (0xC, 0x80000018):
            name_offset = struct.unpack_from("<I", data, offset + 8)[0]
            j = data.index(b"\0", offset + name_offset)
            dylibs.append(data[offset + name_offset:j].decode())
        offset += size
    return binds, dylibs


def exports(path):
    result = subprocess.run(["dyld_info", "-exports", str(path)], capture_output=True, text=True, check=True)
    names = set()
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith("0x"):
            names.add(parts[1])
    return names


def nid_names():
    names = {}
    if NID_DB.exists():
        for line in NID_DB.read_text(errors="replace").splitlines():
            parts = line.split()
            if len(parts) >= 2:
                names.setdefault(parts[0], parts[1])
    return names


def guest_modules(title, resource):
    preload, deferred = {}, {}
    sources = [title / "sce_module", resource / "sce_module"]
    for directory in sources:
        if directory.is_dir():
            for path in sorted(directory.glob("*.prx")):
                preload.setdefault(path.name, path)
    if title.resolve() != resource.resolve():
        for path in sorted(title.glob("*.prx")):
            preload.setdefault(path.name, path)
    for path in sorted(resource.glob("*.prx")):
        if path.name not in preload:
            deferred[path.name] = path
    return preload, deferred


def relink(args, cache, eboot, preload, deferred):
    stamp_path = cache / "relink.json"
    relinker = Path(args.relinker).resolve()
    stamp = {"eboot": sha256(eboot), "relinker": str(relinker), "relinker_mtime": relinker.stat().st_mtime_ns,
             "preload": {name: path.stat().st_size for name, path in preload.items()},
             "deferred": {name: path.stat().st_size for name, path in deferred.items()}}
    out = cache / "out"
    if not args.relink and stamp_path.exists() and (out / "eboot").exists():
        try:
            if json.loads(stamp_path.read_text()) == stamp:
                log(f"relink cached in {cache}")
                return out
        except ValueError:
            pass
    for name in ("src", "mods", "out"):
        shutil.rmtree(cache / name, ignore_errors=True)
    (cache / "src").mkdir(parents=True)
    (cache / "mods" / "sce_module").mkdir(parents=True)
    (cache / "mods" / "deferred").mkdir(parents=True)
    out.mkdir(parents=True)
    started = time.monotonic()
    subprocess.run([sys.executable, str(UNSELF), str(eboot), str(cache / "src" / "input.elf")], check=True, stdout=subprocess.DEVNULL)
    for name, path in preload.items():
        subprocess.run([sys.executable, str(UNSELF), str(path), str(cache / "mods" / "sce_module" / name)], check=True, stdout=subprocess.DEVNULL)
    for name, path in deferred.items():
        subprocess.run([sys.executable, str(UNSELF), str(path), str(cache / "mods" / "deferred" / name)], check=True, stdout=subprocess.DEVNULL)
    command = [str(relinker), "--macos", "--to-rosetta", "--sce-module-path", str(cache / "mods")]
    if deferred:
        command += ["--module-dir", "deferred"]
    command += [str(cache / "src" / "input.elf"), str(out / "eboot")]
    with open(cache / "relink.log", "w") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        print((cache / "relink.log").read_text()[-2000:])
        raise SystemExit(f"relinker failed ({result.returncode}); log: {cache / 'relink.log'}")
    os.chmod(out / "eboot", 0o755)
    stamp_path.write_text(json.dumps(stamp, indent=1))
    log(f"relinked in {time.monotonic() - started:.1f}s ({len(preload)} preloaded, {len(deferred)} deferred modules)")
    return out


def nid_label(symbol, names):
    nid = symbol[1:].split("#")[0]
    return f"{nid} ({names[nid]})" if nid in names else nid


def build_stubs(work, symbols, names, traced=()):
    """One dylib exporting a logging stub (returns 0) for each unresolved symbol,
    and a tracing stub (logs, then jumps to the real definition) for each traced one."""
    work.mkdir(parents=True, exist_ok=True)
    ordered = sorted(symbols)
    traced = sorted(traced)
    asm = ["    .text"]
    for index, symbol in enumerate(ordered):
        asm += [f'    .globl "{symbol}"', f'"{symbol}":', f"    movl ${index}, %eax", "    jmp _aps5_stub_common"]
    for index, symbol in enumerate(traced):
        asm += [f'    .globl "{symbol}"', f'"{symbol}":', f"    movl ${index}, %r11d", "    jmp _aps5_trace_common"]
    asm += ["    .section __DATA,__const", "    .p2align 3"]
    for table, prefix, entries in (("_aps5_stub_names", "Lname", ordered), ("_aps5_trace_names", "Ltrace", traced),
                                   ("_aps5_trace_symbols", "Lsymbol", traced)):
        asm += [f"    .globl {table}", f"{table}:"] + [f"    .quad {prefix}{index}" for index in range(len(entries))] + ["    .quad 0"]
    asm += ["    .p2align 2", "    .globl _aps5_stub_count", "_aps5_stub_count:", f"    .long {len(ordered)}",
            "    .globl _aps5_trace_count", "_aps5_trace_count:", f"    .long {len(traced)}", "    .cstring"]
    for index, symbol in enumerate(ordered):
        asm += [f"Lname{index}:", f'    .asciz "{nid_label(symbol, names)}"']
    for index, symbol in enumerate(traced):
        asm += [f"Ltrace{index}:", f'    .asciz "{nid_label(symbol, names)}"', f"Lsymbol{index}:", f'    .asciz "{symbol}"']
    (work / "stubs.S").write_text("\n".join(asm) + "\n")
    output = work / "aps5_unresolved.dylib"
    subprocess.run(["clang", "-arch", "x86_64", "-O1", "-dynamiclib", "-install_name", str(output), "-o", str(output),
                    str(STUB_RUNTIME), str(work / "stubs.S")], check=True)
    return output


# C++ vtables, typeinfo and VTTs are data: a tracing stub in their place breaks the guest.
DATA_NAME = re.compile(r"^_ZT[VIST]")


def traced_symbols(pattern, imported, unresolved, names):
    """Imported NIDs (bound to a real definition) whose NID or known name matches `pattern`.
    Only functions can be traced: a pattern must not match a data import such as a variable."""
    if not pattern:
        return []
    expression = re.compile(pattern)
    result = []
    for symbol in sorted(imported - set(unresolved) - HOOKS):
        if not NID_SYMBOL.match(symbol):
            continue
        nid = symbol[1:].split("#")[0]
        name = names.get(nid, "")
        if DATA_NAME.match(name):
            continue
        if expression.search(nid) or expression.search(name):
            result.append(symbol)
    return result


def layout(args, cache, out, resource):
    libs_source = Path(args.libs).resolve()
    images = [out / "eboot"] + sorted((out / "app0").rglob("*.guest.prx"))
    imported, rpath_libs = set(), set()
    for image in images:
        binds, dylibs = macho_binds(image)
        imported |= binds
        rpath_libs |= {d.split("/", 1)[1] for d in dylibs if d.startswith("@rpath/")}
    available = set()
    for image in images:
        available |= exports(image)
    libs = out / "libs"
    if libs.is_symlink() or libs.is_file():
        libs.unlink()
    shutil.rmtree(libs, ignore_errors=True)
    libs.mkdir()
    for library in sorted(libs_source.glob("*.prx")):
        (libs / library.name).symlink_to(library)
        if library.name in rpath_libs:
            available |= exports(library)
    missing_libs = sorted(name for name in rpath_libs if not (libs / name).exists())
    stub_work = cache / "stubs"
    stub_work.mkdir(exist_ok=True)
    empty = stub_work / "empty.c"
    empty.write_text("void aps5_rosetta_missing_library(void) {}\n")
    for name in missing_libs:
        subprocess.run(["clang", "-arch", "x86_64", "-dynamiclib", "-install_name", f"@rpath/{name}", "-o", str(libs / name), str(empty)], check=True)
    # Only NIDs: other names (relinker hooks, ___cxa_atexit) come from libkernel or libSystem.
    unresolved = sorted(name for name in imported - available - HOOKS if NID_SYMBOL.match(name))
    names = nid_names()
    traced = traced_symbols(args.trace, imported, unresolved, names)
    if args.trace:
        log(f"tracing {len(traced)} imports matching {args.trace!r}")
    stub_library = build_stubs(stub_work, unresolved, names, traced) if unresolved or traced else None

    # app0: the resource root linked in read-only, plus the relinker outputs.
    app0 = out / "app0"
    app0.mkdir(exist_ok=True)
    for entry in list(app0.rglob("*")):
        if entry.is_symlink():
            entry.unlink()
    for entry in sorted(resource.iterdir()):
        target = app0 / entry.name
        if entry.name == "sce_module" and entry.is_dir():
            target.mkdir(exist_ok=True)
            for module in entry.iterdir():
                if not (target / module.name).exists():
                    (target / module.name).symlink_to(module)
        elif not target.exists():
            target.symlink_to(entry)
    deferred_dir = app0 / "deferred"
    if deferred_dir.is_dir():
        for module in deferred_dir.glob("*.guest.prx"):
            if not (app0 / module.name).exists():
                (app0 / module.name).symlink_to(Path("deferred") / module.name)
    return {"images": len(images), "imports": len(imported), "rpath_libs": len(rpath_libs), "missing_libs": missing_libs,
            "unresolved": unresolved, "stub_library": str(stub_library) if stub_library else None}


def run(args, out, prepared):
    environment = dict(os.environ)
    if not args.abort_on_unimplemented:
        environment.setdefault("ANYPS5_UNIMPLEMENTED", "continue")
    log_path = Path(args.log).resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    command = ["arch", "-x86_64"]
    # /usr/bin/arch is SIP-protected, so DYLD_* variables must go through -e.
    if prepared["stub_library"]:
        command += ["-e", f"DYLD_INSERT_LIBRARIES={prepared['stub_library']}"]
    command += ["./eboot"] + args.guest_args
    started = time.monotonic()
    with open(log_path, "wb") as stream:
        process = subprocess.Popen(command, cwd=out, stdout=stream, stderr=subprocess.STDOUT, env=environment, start_new_session=True)
        timed_out = False
        try:
            status = process.wait(timeout=args.wall)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                status = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                status = process.wait()
    return status, time.monotonic() - started, timed_out, log_path


def summarize(status, seconds, timed_out, log_path, prepared):
    lines = log_path.read_text(errors="replace").splitlines()
    first = lambda key: next((line for line in lines if key in line), None)
    stub_calls = [line for line in lines if "[rosetta] unimplemented import" in line]
    summary = {
        "exit": None if status < 0 else status,
        "signal": signal.Signals(-status).name if status < 0 else None,
        "timed_out": timed_out,
        "seconds": round(seconds, 3),
        "guest_entry": first("[rosetta] guest entry "),
        "guest_returned": first("[rosetta] guest entry returned"),
        "fault": first("[rosetta] guest fault"),
        "uncaught_exception": first("terminating due to uncaught exception") or first("libc++abi: terminating"),
        "dyld_error": first("dyld["),
        "first_unimplemented": stub_calls[0].split("import ", 1)[1] if stub_calls else None,
        "last_imports_called": [line.split("import ", 1)[1] for line in stub_calls[-5:]],
        "distinct_unimplemented_called": len(stub_calls),
        "missing_libs": prepared["missing_libs"],
        "unresolved_imports": len(prepared["unresolved"]),
        "log": str(log_path),
        "log_tail": lines[-12:],
    }
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("title", type=Path)
    parser.add_argument("--resource-root", type=Path)
    parser.add_argument("--build", type=Path, help="x86_64 build dir configured with -DANYPS5_ROSETTA_LIBS=ON")
    parser.add_argument("--libs", help="directory of x86_64 libs/*.prx (default <build>/rosetta/libs)")
    parser.add_argument("--relinker", help="relinker binary (default <build>/core/relinker/relinker)")
    parser.add_argument("--cache", type=Path, help="cache root (default ~/Library/Caches/AnyPS5/rosetta)")
    parser.add_argument("--wall", type=float, default=600)
    parser.add_argument("--log", help="run log (default <cache>/<sha>/run.log)")
    parser.add_argument("--relink", action="store_true")
    parser.add_argument("--abort-on-unimplemented", action="store_true",
                        help="let library functions that are not implemented throw (default: log once and return 0)")
    parser.add_argument("--trace", metavar="REGEX",
                        help="log guest calls to imports whose NID or name matches REGEX (ANYPS5_TRACE_LIMIT calls each, default 16; "
                             "ANYPS5_TRACE_AFTER=SECONDS skips calls made before then; ANYPS5_TRACE_RETURNS=1 also logs each logged call's %%rax)")
    parser.add_argument("guest_args", nargs="*")
    args = parser.parse_args()
    if args.build:
        args.libs = args.libs or str(args.build / "rosetta" / "libs")
        args.relinker = args.relinker or str(args.build / "core" / "relinker" / "relinker")
    if not args.libs or not args.relinker:
        parser.error("pass --build, or both --libs and --relinker")
    title = args.title.resolve()
    resource = (args.resource_root or args.title).resolve()
    eboot = title / "eboot.bin"
    if not eboot.exists():
        eboot = resource / "eboot.bin"
    if not eboot.exists():
        raise SystemExit("no eboot.bin in the title directory or resource root")
    cache = (args.cache or Path.home() / "Library" / "Caches" / "AnyPS5" / "rosetta").expanduser() / sha256(eboot)
    cache.mkdir(parents=True, exist_ok=True)
    args.log = args.log or str(cache / "run.log")
    preload, deferred = guest_modules(title, resource)
    out = relink(args, cache, eboot, preload, deferred)
    prepared = layout(args, cache, out, resource)
    log(f"{prepared['images']} images, {prepared['imports']} imported symbols, {len(prepared['missing_libs'])} libraries stubbed, "
        f"{len(prepared['unresolved'])} imports stubbed; running for up to {args.wall:.0f}s")
    status, seconds, timed_out, log_path = run(args, out, prepared)
    summary = summarize(status, seconds, timed_out, log_path, prepared)
    (cache / "summary.json").write_text(json.dumps(summary, indent=1))
    for key, value in summary.items():
        if key == "log_tail":
            continue
        print(f"{key}: {value}")
    print("log tail:")
    for line in summary["log_tail"]:
        print(f"  {line[:240]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
