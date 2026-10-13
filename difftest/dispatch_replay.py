#!/usr/bin/env python3
"""Dispatch-mode two-sided replay for the compiled hottest functions.

The synthetic image is compiled with `dispatch: true`, so every indirect call/jump
lowers to `NativeAotDispatch(value)`. After loading the dylib the harness registers a
combined table: the image's own functions map to their native addresses, and every
declared target maps to a per-target trampoline that records (target, register file)
and returns a neutral RAX. The x86 reference runs the original bytes at the relinked
base with the same registers, and its output is compared with the AOT output.

usage: PYTHONPATH=core/aot:<work>/tools-py dispatch_replay.py <eboot.elf> <contracts.json> <compile-results.json> <out-dir> [limit]
"""
import ctypes
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

AOT = Path(__file__).resolve().parent.parent / "core" / "aot"
sys.path.insert(0, str(AOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import difftest  # noqa: E402
import replay  # noqa: E402

BASE = replay.BASE

GENERIC_STUB = r"""
#include <stdint.h>
uint64_t aps5_calls[400000][2];
int aps5_call_count;
void aps5_stub(void* rf, uint64_t target);
void aps5_stub(void* rf, uint64_t target) {
    if (aps5_call_count < 400000) {
        aps5_calls[aps5_call_count][0] = target;
        aps5_calls[aps5_call_count][1] = (uint64_t)rf;
        aps5_call_count++;
    }
    ((uint64_t*)rf)[0] = 0;  /* neutral RAX */
}
"""


def build_image(elf, contracts, compiled, work_dir):
    return difftest.Title(elf)


def main():
    if len(sys.argv) < 7:
        raise SystemExit(__doc__)
    elf, contracts_path, compile_path, out_dir = (Path(sys.argv[1]), Path(sys.argv[2]),
                                                  Path(sys.argv[3]), Path(sys.argv[4]))
    limit = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    out_dir.mkdir(parents=True, exist_ok=True)
    contracts = json.loads(contracts_path.read_text())
    results = json.loads(compile_path.read_text())
    compiled = [int(k, 16) for k, v in results.items() if v["status"] == "compiled"]
    if limit:
        compiled = compiled[:limit]
    target_sets = contracts["target_sets"]
    by_address = {spec["address"]: spec for spec in contracts["functions"]}
    targets = set()
    internal = set(compiled)
    for address in compiled:
        spec = by_address.get(address, {})
        for record in spec.get("indirect_calls", []) + spec.get("indirect_jumps", []):
            listed = record.get("targets")
            targets.update(listed if listed is not None else target_sets[record["target_set"]])
    # Direct call/jmp targets that dispatch mode routes through the table too.
    title0 = difftest.Title(elf)
    sizes0 = difftest.fde_sizes(Path(sys.argv[6]))
    for address in compiled:
        size = sizes0.get(address, 0)
        code0 = title0.code(address, size) if size else None
        if code0 is None:
            continue
        for target in difftest.direct_targets(code0, address):
            if target not in internal:
                targets.add(target)
    print(f"compiled {len(compiled)}, dispatch targets {len(targets)}", flush=True)

    sizes = difftest.fde_sizes(Path(sys.argv[6]))
    title = difftest.Title(elf)
    codes = [(a, title.code(a, sizes[a])) for a in compiled if sizes.get(a) and title.code(a, sizes[a])]
    referenced = {}
    for a, code in codes:
        for target, size in difftest.references(code, a).items():
            referenced[target] = max(referenced.get(target, 1), size)
    image, reduced = difftest.build_synthetic(codes, referenced, uniform=True, dispatch=True)
    work = out_dir / "image"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "input.elf"
    source.write_bytes(image)
    from compiler import Image, compile_image
    from toolchain import build_native_object
    ir, report = compile_image(Image(source, reduced))
    ir_path = work / "image.ll"
    ir_path.write_text(ir)
    object_file = work / "image.o"
    build_native_object(ir, object_file)
    dispatch_source = Path(__file__).resolve().parent.parent / "core" / "libs" / "native" / "NativeAotDispatch.cpp"
    dispatch_object = work / "dispatch.o"
    subprocess.run(["xcrun", "clang++", "-arch", "arm64", "-O2", "-c", str(dispatch_source), "-o", str(dispatch_object)],
                   check=True, capture_output=True)
    dylib = work / "image.dylib"
    subprocess.run(["xcrun", "clang++", "-arch", "arm64", "-dynamiclib", str(object_file), str(dispatch_object), "-o", str(dylib)],
                   check=True, capture_output=True)

    assembly = work / "stubs.s"
    with assembly.open("w") as stream:
        stream.write(".text\n")
        for target in sorted(targets):
            stream.write(f".globl _aps5_stub_{target:x}\n_aps5_stub_{target:x}:\n")
            stream.write(f"  movz x1, #{target & 0xffff}\n")
            for shift in (16, 32, 48):
                chunk = (target >> shift) & 0xffff
                if chunk:
                    stream.write(f"  movk x1, #{chunk}, lsl #{shift}\n")
            stream.write("  b _aps5_stub\n")
    stub_source = work / "stubs.c"
    stub_source.write_text(GENERIC_STUB)
    stubs = work / "stubs.dylib"
    subprocess.run(["xcrun", "clang", "-arch", "arm64", "-O2", "-dynamiclib", str(stub_source), str(assembly), "-o", str(stubs)],
                   check=True, capture_output=True)

    print("loading dylibs", flush=True)
    library = ctypes.CDLL(str(dylib))
    stub_library = ctypes.CDLL(str(stubs))
    print("loaded", flush=True)
    entries = []
    for address in compiled:
        try:
            native = ctypes.cast(getattr(library, f"sub_{address:x}"), ctypes.c_void_p).value
        except AttributeError:
            continue
        entries.append((address, native))
    for target in sorted(targets):
        try:
            native = ctypes.cast(getattr(stub_library, f"aps5_stub_{target:x}"), ctypes.c_void_p).value
        except AttributeError:
            continue
        entries.append((target, native))
    entries.sort()
    table = (ctypes.c_uint64 * (2 * len(entries)))()
    for index, (guest, native) in enumerate(entries):
        table[2 * index] = guest
        table[2 * index + 1] = native
    libc = ctypes.CDLL(None)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
    for region in (0x60000000, 0x70000000):
        got = libc.mmap(ctypes.c_void_p(BASE + region), 0x100000, 1 | 2, 0x02 | 0x1000 | 0x10, -1, 0)
        print(f'  mmap {BASE+region:#x} -> {got}', flush=True)
    print(f"registering {len(entries)} entries", flush=True)
    register = library.NativeAotRegisterDispatch_nid_no_patch
    register.restype = None
    register.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    register(ctypes.byref(table), ctypes.c_size_t(len(entries)))
    print("registered", flush=True)

    print('building x86 runner', flush=True)
    runner = replay.build_x86_runner(work)
    print('runner built', flush=True)
    matched = mismatched = x86_fault = arm_fault = 0
    details = []
    for address in compiled:
        registers = {item: BASE + 0x60000000 for item in replay.UNIFORM_GPRS if item != "rsp"}
        registers["rsp"] = BASE + 0x700FFFF8
        result_path = work / f"arm_{address:x}.bin"
        pid = os.fork()
        if pid == 0:
            try:
                regfile = (ctypes.c_uint64 * 80)()
                for index, register in enumerate(replay.UNIFORM_GPRS):
                    regfile[index] = registers.get(register, 0)
                function = getattr(library, f"sub_{address:x}")
                function.restype = None
                function.argtypes = [ctypes.c_void_p]
                function(ctypes.byref(regfile))
                result_path.write_bytes(regfile[0].to_bytes(8, "little"))
                os._exit(0)
            except BaseException:
                os._exit(1)
        _, status = os.waitpid(pid, 0)
        if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
            arm_fault += 1
            continue
        arm_rax = int.from_bytes(result_path.read_bytes(), "little")
        x86, code = replay.run_x86(work, runner, image, address, registers, bytes(1 << 16))
        if x86 is None:
            x86_fault += 1
            continue
        if arm_rax == x86["rax"]:
            matched += 1
        else:
            mismatched += 1
            details.append({"function": hex(address), "arm64_rax": arm_rax, "x86_rax": x86["rax"]})
    report_out = {"compiled": len(compiled), "matched": matched, "mismatched": mismatched,
                  "x86_fault": x86_fault, "arm64_fault": arm_fault, "mismatches": details[:100]}
    (out_dir / "dispatch-replay.json").write_text(json.dumps(report_out, indent=1))
    print(f"matched={matched} mismatched={mismatched} x86_fault={x86_fault} arm64_fault={arm_fault}")


if __name__ == "__main__":
    sys.exit(main())
