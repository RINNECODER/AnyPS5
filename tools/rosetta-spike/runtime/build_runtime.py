#!/usr/bin/env python3
"""Build the spike runtime libraries beside a title relinked with --macos.

usage: build_runtime.py <relinked-output-dir> <work-dir>

Reads the bind opcodes of the executable and of app0/sce_module/*.prx,
generates one logging stub per imported symbol into libkernel.prx (with the
hooks from spike_rt.c / spike_tls.S), and an empty dylib for every other
@rpath library the images load, so dyld can start the title under Rosetta.
"""
import pathlib
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
HOOKS = {
    "_Aps5StartGuest_nid_no_patch", "_Aps5GuestTlsAllocate_nid_no_patch", "_Aps5GuestTlsRegister_nid_no_patch",
    "___aps5_get_pending_module_args_nid_no_patch", "___aps5_set_module_init_result_nid_no_patch", "___cxa_atexit",
}


def uleb(data, i):
    value = shift = 0
    while True:
        b = data[i]
        i += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if b < 0x80:
            return value, i


def macho_info(path):
    data = path.read_bytes()
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", data, 0)
    assert magic == 0xFEEDFACF, path
    off = 32
    binds, dylibs = set(), []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd in (0x22, 0x80000022):
            bind_off, bind_size = struct.unpack_from("<II", data, off + 16)
            lazy_off, lazy_size = struct.unpack_from("<II", data, off + 32)
            for start, length in ((bind_off, bind_size), (lazy_off, lazy_size)):
                i, end = start, start + length
                while i < end:
                    op = data[i] & 0xF0
                    i += 1
                    if op == 0x40:
                        j = data.index(b"\0", i)
                        binds.add(data[i:j].decode())
                        i = j + 1
                    elif op in (0x10, 0x60, 0x70, 0xA0):
                        _, i = uleb(data, i)
                    elif op == 0xC0:
                        _, i = uleb(data, i)
                        _, i = uleb(data, i)
        elif cmd in (0xC, 0x80000018):
            name_off = struct.unpack_from("<I", data, off + 8)[0]
            j = data.index(b"\0", off + name_off)
            dylibs.append(data[off + name_off:j].decode())
        off += size
    return binds, dylibs


def run(*cmd):
    subprocess.run(cmd, check=True)


def main():
    out, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    images = [out / "eboot"] + sorted((out / "app0" / "sce_module").glob("*.prx"))
    symbols, rpath_libs = set(), set()
    for image in images:
        binds, dylibs = macho_info(image)
        symbols |= binds
        rpath_libs |= {d.split("/", 1)[1] for d in dylibs if d.startswith("@rpath/")}
    stubs = sorted(symbols - HOOKS)
    print(f"images={len(images)} imported_symbols={len(symbols)} stubs={len(stubs)} rpath_libs={len(rpath_libs)}")
    asm = ["    .text"]
    for index, name in enumerate(stubs):
        asm += [f'    .globl "{name}"', f'"{name}":', f"    movl ${index}, %eax", "    jmp _aps5_stub_common"]
    asm += ["    .section __DATA,__const", "    .p2align 3", "    .globl _aps5_stub_names", "_aps5_stub_names:"]
    asm += [f"    .quad Lname{index}" for index in range(len(stubs))]
    asm += ["    .globl _aps5_stub_count", "    .p2align 2", "_aps5_stub_count:", f"    .long {len(stubs)}", "    .cstring"]
    for index, name in enumerate(stubs):
        asm += [f"Lname{index}:", f'    .asciz "{name}"']
    (work / "stubs.S").write_text("\n".join(asm) + "\n")
    libs = out / "libs"
    libs.mkdir(exist_ok=True)
    base = ["clang", "-arch", "x86_64", "-O1", "-dynamiclib", "-Wl,-undefined,dynamic_lookup"]
    run(*base, "-install_name", "@rpath/libkernel.prx", "-o", str(libs / "libkernel.prx"),
        str(HERE / "spike_rt.c"), str(HERE / "spike_tls.S"), str(work / "stubs.S"))
    empty = work / "empty.c"
    empty.write_text("void aps5_spike_empty(void) {}\n")
    for name in sorted(rpath_libs - {"libkernel.prx"}):
        run(*base, "-install_name", f"@rpath/{name}", "-o", str(libs / name), str(empty))
    print(f"runtime written to {libs}")


if __name__ == "__main__":
    main()
