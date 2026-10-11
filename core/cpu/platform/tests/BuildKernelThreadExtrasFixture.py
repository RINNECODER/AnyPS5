"""Preserve genuine linked FUNC/PLT imports; select the thread extras fixture only."""
import importlib.util
import struct
from pathlib import Path
import sys

sys.dont_write_bytecode = True
path = Path(__file__).with_name("BuildKernelMutexThreadsFixture.py")
spec = importlib.util.spec_from_file_location("kernel_thread_extras_packager", path)
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
KERNEL = {
    "thread_create": "6UgtwV+0zb4", "thread_yield": "T72hz6ffq08", "thread_join": "onNY9Byn-W8",
    "thread_self": "aI+OeCz8xrQ", "process_exit": "6Z83sYWFlA8",
    "attr_init": "nsYoNRywwNg", "attr_destroy": "62KCwEMmzcM", "attr_policy": "4+h9EzwKF4I",
    "rw_init": "6ULAa0fq4jA", "rw_destroy": "BB+kb08Tl9A", "rw_wrlock": "mqdNorrB+gI",
    "rw_rdlock": "Ox9i0c7L5w0", "rw_trywrlock": "bIHoZCTomsI", "rw_tryrdlock": "XD3mDeybCnk",
    "rw_timedwrlock": "adh--6nIqTk", "rw_timedrdlock": "iPtZRWICjrM", "rw_unlock": "+L98PIbGttk",
    "once": "14bOACANTBo", "key_create": "geDaqgH9lTg", "key_delete": "PrdHuuDekhY",
    "set_specific": "+BzXYkqYeLE", "get_specific": "eoht7mQOCmo",
    "sync_wait": "Hc4CaR6JBL0", "sync_wait32": "B2n8aDorSH4", "sync_wait64": "PZQhiiLXRFs",
    "sync_wake": "q2y-wDIVWZA",
    "attr_setstacksize": "UTXzJbWhhTE", "attr_getstacksize": "-fA+7ZlGDQs",
    "attr_setguardsize": "El+cQ20DynU", "attr_getguardsize": "txHtngJ+eyc",
    "attr_setdetachstate": "-Wreprtu0Qs", "attr_getdetachstate": "JaRMy+QcpeU",
    "attr_setaffinity": "3qxgM4ezETA", "attr_getaffinity": "8+s5BzZjxSg",
    "attr_getschedpolicy": "NMyIQ9WgWbU", "attr_getinheritsched": "lpMP8HhkBbg",
    "attr_setstack": "Bvn74vj6oLo", "attr_get": "x1X76arYMxU",
    "thread_detach": "4qGrR6eoP9Y", "thread_getname": "How7B8Oet6k", "thread_rename": "GBUY7ywdULE",
    "thread_setprio": "W0Hpm2X0uPE", "thread_getprio": "1tKyG7RlMJo",
    "thread_setaffinity": "bt3CTBKmGyI", "thread_getaffinity": "rcrVFJsQWRY",
    "thread_cancel": "qBDmpCyGssE", "thread_testcancel": "LapIb799SSE",
    "thread_getthreadid": "EI-5-jlq2dE",
    "module_from_addr": "f7KBOafysXo", "module_for_unwind": "RpQJJVKTiFM",
    "debug_raise": "OMDRKKAZ8I4", "debug_raise_release": "zE-wXIZjLoM", "debug_cpp_info": "cfwBSQyr5Ys",
    "load_start": "wzvqT4UqKX8", "stop_unload": "QKd0qM58Qes", "module_dlsym": "LwG8g3niqwA",
    "is_trinity": "tU5e3f9gSiU", "uuid_create": "Xjoosiw+XPI", "atexit_inc": "Tz4RNUCBbGI",
    "atexit_dec": "8OnWXlgQlvo", "get_pid": "HoLVWNanBBc", "psem_init": "pDuPEf3m4fI", "psem_post": "IKP8typ0QUk",
    "psem_wait": "YCV5dGGBcCo", "psem_destroy": "cDW233RAwWo", "psem_timedwait": "w5IHyvahg-o",
    "get_rusage": "hHlZQUnlxSM", "sig_set": "VADc3MNQ3cM", "sig_mask": "6xVpy0Fdq+I",
    "phdr_match": "Fjc4-n1+y2g", "err_loc": "9BcDykPmo1I", "clock_now": "QBi7HCK03hw",
}
fixture.IMPORTS = {name: (nid, "libkernel", 1, "B") for name, nid in KERNEL.items()}
fixture.EXPORTS = {"_start": 2, "KernelThreadExtrasReceipt": 1}
PT_GNU_EH_FRAME, PT_GNU_STACK = 0x6474E550, 0x6474E551


def program_headers(image):
    offset, = struct.unpack_from("<Q", image, 32)
    size, count = struct.unpack_from("<HH", image, 54)
    return [(offset + i * size, struct.unpack_from("<I", image, offset + i * size)[0]) for i in range(count)]


def keep_eh_frame_header(linked, output):
    """Retail modules carry PT_GNU_EH_FRAME; the packager drops it, so the
    linker's header replaces the packaged PT_GNU_STACK entry (load layout is
    unchanged, so the linker's offsets stay valid)."""
    source = Path(linked).read_bytes()
    image = bytearray(Path(output).read_bytes())
    header, = [at for at, kind in program_headers(source) if kind == PT_GNU_EH_FRAME]
    stack, = [at for at, kind in program_headers(image) if kind == PT_GNU_STACK]
    image[stack:stack + 56] = source[header:header + 56]
    Path(output).write_bytes(image)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: BuildKernelThreadExtrasFixture.py linked.elf output.elf")
    fixture.package(*sys.argv[1:])
    keep_eh_frame_header(*sys.argv[1:])
