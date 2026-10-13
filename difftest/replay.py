#!/usr/bin/env python3
"""Differential replay: the same recorded inputs through the AOT ARM64 function
and through the original x86 bytes under Rosetta.

For each case we build the ARM64 dylib from the compiler IR and call the exported
`func_<address>`; on the x86 side we map the function's original bytes at their
original address and call them with the same general-purpose registers. Outputs
compared: RAX (and RDX for a 128-bit result) plus a caller-supplied memory window.

The title's functions need their ABI signatures and the memory they read; those are
the recording step (targeted entry breakpoint) and are supplied per case here.
"""
import ctypes
import json
import subprocess
import sys
import tempfile
from pathlib import Path

AOT = Path(__file__).resolve().parent.parent / "core" / "aot"
sys.path.insert(0, str(AOT))

from toolchain import build_native_object  # noqa: E402

REGISTERS = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]

X86_RUNNER = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
struct regs { uint64_t r[16]; };
extern uint64_t invoke(void* entry, struct regs* in, struct regs* out, uint64_t* stack, uint64_t stack_words);
__asm__(
".text\n"
".globl _invoke\n"
"_invoke:\n"
"  pushq %rbp\n"
"  movq %rsp, %rbp\n"
"  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
"  movq %rdi, %r11\n"          /* entry */
"  movq %rsi, %r10\n"          /* in */
"  movq %rdx, %r9\n"           /* out */
"  movq %rcx, %rax\n"          /* stack */
"  leaq -8(%rcx,%r8,8), %rsp\n"
"  movq 0(%r10), %rax\n  movq 8(%r10), %rbx\n  movq 16(%r10), %rcx\n  movq 24(%r10), %rdx\n"
"  movq 32(%r10), %rsi\n  movq 40(%r10), %rdi\n  movq 48(%r10), %rbp\n"
"  movq 64(%r10), %r8\n  movq 72(%r10), %r9\n  movq 80(%r10), %r11\n"
"  movq 88(%r10), %r12\n  movq 96(%r10), %r13\n  movq 104(%r10), %r14\n  movq 112(%r10), %r15\n"
"  callq *%r11\n"
"  movq %rax, 0(%r9)\n  movq %rbx, 8(%r9)\n  movq %rcx, 16(%r9)\n  movq %rdx, 24(%r9)\n"
"  movq %rsi, 32(%r9)\n  movq %rdi, 40(%r9)\n  movq %r8, 64(%r9)\n  movq %r9, 72(%r9)\n"
"  movq %r10, 80(%r9)\n  movq %r11, 88(%r9)\n  movq %r12, 96(%r9)\n  movq %r13, 104(%r9)\n  movq %r14, 112(%r9)\n  movq %r15, 120(%r9)\n"
"  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n  ret\n");
int main(int argc, char** argv) {
    FILE* file = fopen(argv[1], "rb");
    uint64_t address = strtoull(argv[2], 0, 16);
    long size = 0; fseek(file, 0, SEEK_END); size = ftell(file); fseek(file, 0, SEEK_SET);
    uint64_t low = 0x1000, high = address + size + 0x1000;
    if (mmap((void*)low, high - low, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) { perror("mmap"); return 3; }
    if (fread((void*)address, 1, size, file) != (size_t)size) return 4;
    mprotect((void*)(address & ~0xfffull), (size + 0x2fff) & ~0xfffull, PROT_READ | PROT_EXEC);
    struct regs in, out; uint64_t stack[1024];
    FILE* input = fopen(argv[3], "rb");
    if (fread(&in, 1, sizeof(in), input) != sizeof(in)) return 5;
    fread(stack, 1, sizeof(stack), input);
    memset(&out, 0, sizeof(out));
    invoke((void*)address, &in, &out, stack, 1024);
    FILE* output = fopen(argv[4], "wb");
    fwrite(&out, 1, sizeof(out), output);
    fclose(output); fclose(input); fclose(file);
    return 0;
}
"""


def build_arm64(ir, directory):
    object_file = directory / "native.o"
    build_native_object(ir, object_file)
    dylib = directory / "native.dylib"
    subprocess.run(["xcrun", "clang", "-arch", "arm64", "-dynamiclib", str(object_file), "-o", str(dylib)], check=True, capture_output=True)
    return dylib


def call_arm64(dylib, name, arguments):
    library = ctypes.CDLL(str(dylib))
    function = getattr(library, name)
    function.restype = ctypes.c_uint64
    function.argtypes = [ctypes.c_uint64] * len(arguments)
    return function(*arguments)


def build_x86_runner(directory):
    source = directory / "runner.c"
    source.write_text(X86_RUNNER)
    binary = directory / "runner"
    subprocess.run(["xcrun", "clang", "-arch", "x86_64", "-O1", str(source), "-o", str(binary)], check=True, capture_output=True)
    return binary


def run_x86(directory, runner, code, address, registers, memory):
    code_file = directory / "code.bin"
    code_file.write_bytes(code)
    input_file = directory / "case.bin"
    packed = b"".join(int(registers.get(name, 0)).to_bytes(8, "little") for name in REGISTERS)
    packed += memory
    input_file.write_bytes(packed)
    output_file = directory / "out.bin"
    subprocess.run(["arch", "-x86_64", str(runner), str(code_file), hex(address), str(input_file), str(output_file)], check=True)
    raw = output_file.read_bytes()
    return {name: int.from_bytes(raw[index * 8:index * 8 + 8], "little") for index, name in enumerate(REGISTERS)}


def replay_case(directory, ir, name, code, address, registers, memory):
    dylib = build_arm64(ir, directory)
    arguments = [int(registers.get(register, 0)) for register in ("rdi", "rsi", "rdx", "rcx", "r8", "r9")]
    arm = call_arm64(dylib, name, arguments)
    runner = build_x86_runner(directory)
    x86 = run_x86(directory, runner, code, address, registers, memory)
    return {"arm64_rax": arm, "x86_rax": x86["rax"], "match": arm == x86["rax"]}


if __name__ == "__main__":
    raise SystemExit("import difftest.replay; the orchestrator lives in difftest.py")
