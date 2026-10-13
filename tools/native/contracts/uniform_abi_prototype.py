#!/usr/bin/env python3
"""Prototype: compile one internal function with a uniform guest-register-state ABI.

Demonstrates the minimal upstream change described in lanes/gaps/REPORT5.md: instead of a
per-function typed ABI, every internal function takes a pointer to a 32-slot guest
register file (16 GPRs + 16 XMMs) and reads/writes it, so no per-function result/parameter
types are needed. The HLE import boundary keeps its typed ABI.

The upstream compiler is imported read-only from the vendored core/aot (lane vtables) and
patched in-process (``abi.signature`` and ``compiler.Function``); no upstream file is
modified.

usage: uniform_abi_prototype.py --aot-root <core/aot> [--workdir DIR] [--arm64]
"""
import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

REGISTER_ORDER = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi'] + ['r%d' % i for i in range(8, 16)]
VECTOR_ORDER = ['xmm%d' % i for i in range(16)]
REGFILE_TYPE = '{ [16 x i64], [16 x i128] }'


def fixture(code):
    """A minimal x86-64 ELF with one executable function and no imports (upstream test style)."""
    image = bytearray(0x5200)
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    struct.pack_into('<16sHHIQQQIHHHHHH', image, 0, ident, 3, 62, 1, 0x1000, 64, 0x5000, 0, 64, 56, 3, 64, 5, 4)
    struct.pack_into('<IIQQQQQQ', image, 64, 1, 5, 0x1000, 0x1000, 0, len(code), len(code), 0x1000)
    struct.pack_into('<IIQQQQQQ', image, 120, 1, 6, 0x2000, 0x2000, 0, 0x3000, 0x3000, 0x1000)
    image[0x1000:0x1000 + len(code)] = code
    strings = bytearray(b'\0')
    tags = [(6, 0x2100), (11, 24), (5, 0x2200), (10, len(strings)), (7, 0x2380), (8, 0), (9, 24), (0, 0)]
    for index, tag in enumerate(tags):
        struct.pack_into('<qQ', image, 0x2400 + index * 16, *tag)
    struct.pack_into('<IIQQQQQQ', image, 176, 2, 6, 0x2400, 0x2400, 0, len(tags) * 16, len(tags) * 16, 8)
    names = b'\0.dynsym\0.dynstr\0.dynamic\0.shstrtab\0'
    image[0x2500:0x2500 + len(names)] = names
    for index, entry in enumerate([
        (1, 11, 2, 0x2100, 0x2100, 24, 2, 1, 8, 24),
        (9, 3, 2, 0x2200, 0x2200, len(strings), 0, 0, 1, 0),
        (17, 6, 3, 0x2400, 0x2400, len(tags) * 16, 2, 0, 8, 16),
        (26, 3, 0, 0, 0x2500, len(names), 0, 0, 1, 0)], 1):
        struct.pack_into('<IIQQQQIIQQ', image, 0x5000 + index * 64, *entry)
    contracts = {'schema': 1, 'sha256': hashlib.sha256(image).hexdigest(),
                 'functions': [{'name': 'sub_1000', 'address': 0x1000, 'size': len(code),
                                'uniform': True, 'result': 'void', 'parameters': []}],
                 'imports': {}, 'data': [{'address': 0x2000, 'size': 0x3000}], 'pointer_relocations': []}
    return bytes(image), contracts


def install_uniform_patch(aot_root):
    sys.path.insert(0, aot_root)
    import abi
    import compiler

    orig_signature = abi.signature

    def signature(contract):
        if contract.get('uniform'):
            return 'void', []
        return orig_signature(contract)

    abi.signature = signature

    orig_locations = abi.locations

    def locations(parameters):
        return orig_locations(parameters)

    abi.locations = locations

    orig_init = compiler.Function.__init__

    def __init__(self, image, contract):
        orig_init(self, image, contract)
        if contract.get('uniform'):
            self.uniform = True
            # the whole guest register file is defined at entry (pass-through register state)
            self.argument_locations = (
                [{'kind': 'register', 'name': r, 'bits': 64, 'type': 'ptr'} for r in REGISTER_ORDER]
                + [{'kind': 'register', 'name': 'xmm%d' % i, 'bits': 128, 'type': 'f64'} for i in range(16)]
                + [{'kind': 'stack', 'offset': 8 + 8 * i, 'bits': 64, 'type': 'i64'} for i in range(32)])
            self.stack_end = 8 * 32

    compiler.Function.__init__ = __init__

    def uniform_lower(self):
        lines = [f'define void @{self.contract["name"]}(ptr %rf) uwtable {{', 'entry:']
        for reg in compiler.REGISTERS:
            lines.append(f'  %{reg} = alloca i64, align 8')
        for reg in compiler.VECTOR_STORAGE:
            lines.append(f'  %{reg} = alloca i128, align 16')
        for flag in ('zf', 'sf', 'cf', 'of', 'pf'):
            lines.append(f'  %{flag} = alloca i1, align 1')
        self.lines = lines
        self.serial = 0
        for i, reg in enumerate(REGISTER_ORDER):
            gep = self.emit('getelementptr inbounds ' + REGFILE_TYPE + ', ptr %rf, i64 0, i32 0, i64 ' + str(i))
            value = self.emit(f'load i64, ptr {gep}, align 8')
            lines.append(f'  store i64 {value}, ptr %{reg}, align 8')
        for i, reg in enumerate(VECTOR_ORDER):
            gep = self.emit('getelementptr inbounds ' + REGFILE_TYPE + ', ptr %rf, i64 0, i32 1, i64 ' + str(i))
            value = self.emit(f'load i128, ptr {gep}, align 16')
            lines.append(f'  store i128 {value}, ptr %{reg}, align 16')
        lines.append(f'  br label %b{min(self.instructions):x}')
        for address in sorted(self.instructions):
            ins = self.instructions[address]
            args, op = ins.operands, ins.mnemonic
            self.lines.append(f'b{address:x}:')
            if op in ('mov', 'movabs'):
                self.write(ins, args[0], self.read(ins, args[1], args[0].size * 8))
            elif op in ('add', 'sub', 'and', 'or', 'xor'):
                bits = args[0].size * 8
                left = self.read(ins, args[0], bits)
                right = self.read(ins, args[1], bits)
                self.write(ins, args[0], self.emit(f'{op} i{bits} {left}, {right}'))
            elif op == 'lea':
                self.write(ins, args[0], self.effective_integer(ins, args[1]))
            elif op == 'ret':
                gep = self.emit('getelementptr inbounds ' + REGFILE_TYPE + ', ptr %rf, i64 0, i32 0, i64 0')
                value = self.emit('load i64, ptr %rax, align 8')
                self.lines.append(f'  store i64 {value}, ptr {gep}, align 8')
                self.lines.append('  ret void')
                continue
            elif op != 'nop':
                raise SystemExit('prototype does not lower %s' % op)
            self.lines.append(f'  br label %b{address + ins.size:x}')
        self.lines.append('}')
        return '\n'.join(self.lines)

    orig_lower = compiler.Function.lower

    def lower(self):
        if getattr(self, 'uniform', False):
            return uniform_lower(self)
        return orig_lower(self)

    compiler.Function.lower = lower
    return compiler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--aot-root', required=True)
    ap.add_argument('--workdir', default='.')
    ap.add_argument('--arm64', action='store_true')
    args = ap.parse_args()

    compiler = install_uniform_patch(args.aot_root)
    # mov rax, rdi ; add rax, rsi ; ret   (internal function, uniform register-file ABI)
    code = bytes.fromhex('4889f84801f0c3')
    raw, contracts = fixture(code)
    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    elf = work / 'prototype.elf'
    elf.write_bytes(raw)
    ir, report = compiler.compile_image(compiler.Image(elf, contracts))
    out = work / 'uniform_prototype.ll'
    out.write_text(ir)
    print(ir)
    print('# emitted function signature:', [line for line in ir.splitlines() if line.startswith('define')])
    if args.arm64:
        optimized = work / 'uniform_prototype.opt.ll'
        subprocess.run(['clang', '-arch', 'arm64', '-O3', '-S', '-emit-llvm', str(out), '-o', str(optimized)],
                       check=True, capture_output=True)
        text = optimized.read_text()
        print('# optimized ARM64 signature:', [line for line in text.splitlines() if line.startswith('define')])
        asm = work / 'uniform_prototype.s'
        subprocess.run(['clang', '-arch', 'arm64', '-O3', '-S', str(out), '-o', str(asm)],
                       check=True, capture_output=True)
        body = []
        for line in asm.read_text().splitlines():
            if line.startswith('_sub_1000:'):
                body = [line]
            elif body:
                body.append(line)
                if line.strip() == '.cfi_endproc':
                    break
        print('\n'.join(body))


if __name__ == '__main__':
    main()
