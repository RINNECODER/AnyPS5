#!/usr/bin/env python3
"""Run the dumped x86-64 kernel under Unicorn (QEMU TCG) and report time.

usage: bench_tcg.py <kernel.bin> <iters> [--count-insns]

Block-chained TCG with no hooks is the best case for a TCG engine; the
AnyPS5 TCG runtime steps one instruction at a time and is slower than this.
"""
import struct
import sys
import time

from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_RAX, UC_X86_REG_RDI, UC_X86_REG_RSI,
                               UC_X86_REG_RSP, UC_X86_REG_XMM0)

CODE, BUF, STACK, RET = 0x10000, 0x200000, 0x800000, 0x900000

code = open(sys.argv[1], "rb").read()
iters = int(sys.argv[2], 0)
count = "--count-insns" in sys.argv

uc = Uc(UC_ARCH_X86, UC_MODE_64)
uc.mem_map(CODE, 0x1000)
uc.mem_write(CODE, code[:0x1000])
uc.mem_map(BUF, 0x4000)
uc.mem_write(BUF, b"".join(struct.pack("<I", (i * 2654435761) & 0xFFFFFFFF) for i in range(4096)))
uc.mem_map(STACK - 0x10000, 0x10000)
uc.mem_map(RET, 0x1000)
rsp = STACK - 0x100
uc.mem_write(rsp, struct.pack("<Q", RET))
uc.reg_write(UC_X86_REG_RSP, rsp)
uc.reg_write(UC_X86_REG_RDI, iters)
uc.reg_write(UC_X86_REG_RSI, BUF)
uc.reg_write(UC_X86_REG_XMM0, struct.unpack("<Q", struct.pack("<d", 0.5))[0])

insns = [0]
if count:
    def hook(_uc, _addr, _size, _data):
        insns[0] += 1
    uc.hook_add(UC_HOOK_CODE, hook)

t = time.perf_counter()
uc.emu_start(CODE, RET)
s = time.perf_counter() - t
r = uc.reg_read(UC_X86_REG_RAX)
print(f"iters={iters} result={r:016x} seconds={s:.4f}" + (f" insns={insns[0]}" if count else ""))
