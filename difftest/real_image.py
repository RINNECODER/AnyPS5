#!/usr/bin/env python3
"""The real module image, mapped for the differential harness.

The synthetic contract only maps the closure's own code and the data regions it references; with
``guest_data`` the compiler instead references title data at ``guest_base + vaddr``. Both runners
must therefore map the real eboot's PT_LOAD segments at ``BASE + vaddr`` and have the profile's
pointer relocations already applied, so a relocated function pointer is the guest address the
native dispatch table is keyed by (``guest_base + target``).

Nothing here is title-specific: the ELF path comes from the caller.
"""
import ctypes
import hashlib
import io
import struct
from pathlib import Path

from elftools.elf.elffile import ELFFile

BASE = 0x140000000
PAGE = 0x1000


def sha256(elf_path):
    return hashlib.sha256(Path(elf_path).read_bytes()).hexdigest()


def segments(elf_path):
    """[(vaddr, filesz, memsz, data, flags)] for every PT_LOAD with a nonzero memory size."""
    raw = Path(elf_path).read_bytes()
    elf = ELFFile(io.BytesIO(raw))
    found = []
    for segment in elf.iter_segments():
        if segment["p_type"] != "PT_LOAD" or segment["p_memsz"] == 0:
            continue
        offset = segment["p_offset"]
        found.append((segment["p_vaddr"], segment["p_filesz"], segment["p_memsz"],
                      raw[offset:offset + segment["p_filesz"]], segment["p_flags"]))
    return found


def patch(segments_list, relocations, base=BASE):
    """Apply (slot, target) pointer relocations: the slot holds the guest address of the target."""
    import bisect
    starts = [segment[0] for segment in segments_list]
    data = [bytearray(segment[3]) for segment in segments_list]
    for slot, target in relocations:
        index = bisect.bisect_right(starts, slot) - 1
        if index < 0:
            continue
        offset = slot - starts[index]
        if offset + 8 > segments_list[index][1]:
            continue
        data[index][offset:offset + 8] = (base + target).to_bytes(8, "little")
    return [(segment[0], segment[1], segment[2], bytes(data[index]), segment[4])
            for index, segment in enumerate(segments_list)]


def blob(segments_list):
    """The x86 runner's segment stream (BASE-relative vaddr, as the synthetic path writes it)."""
    stream = io.BytesIO()
    stream.write(struct.pack("<I", len(segments_list)))
    for vaddr, filesz, memsz, data, _flags in segments_list:
        stream.write(struct.pack("<QQQ", vaddr, filesz, memsz))
        stream.write(data)
    return stream.getvalue()


def map_segments(segments_list, base=BASE):
    """Map the union of the segments at BASE + vaddr and copy the bytes in (child process only)."""
    if not segments_list:
        return
    low = min(vaddr & ~(PAGE - 1) for vaddr, _f, _m, _d, _x in segments_list)
    high = max(vaddr + memsz for vaddr, _f, memsz, _d, _x in segments_list)
    libc = ctypes.CDLL(None)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
                          ctypes.c_int, ctypes.c_long]
    pointer = libc.mmap(ctypes.c_void_p(base + low), high - low, 3, 0x02 | 0x1000 | 0x10, -1, 0)
    if pointer in (None, ctypes.c_void_p(-1).value, (1 << 64) - 1):
        raise RuntimeError(f"cannot map the real image at {base + low:#x}")
    for vaddr, filesz, _memsz, data, _flags in segments_list:
        if filesz:
            ctypes.memmove(base + vaddr, data, filesz)
