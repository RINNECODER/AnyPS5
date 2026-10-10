"""Package the linked public mutex/condition semantics fixture (issue #280) with libkernel-scoped imports."""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("kernel_semantics_linker", Path(__file__).with_name("BuildKernelMutexThreadsFixture.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
fixture.IMPORTS = {name: fixture.IMPORTS[name] for name in (
    "scePthreadMutexInit", "scePthreadMutexLock", "scePthreadMutexTrylock", "scePthreadMutexUnlock",
    "scePthreadMutexDestroy", "scePthreadMutexattrInit", "scePthreadMutexattrSettype", "scePthreadMutexattrDestroy",
    "thread_create", "thread_join", "thread_yield", "thread_self", "process_exit")}
fixture.IMPORTS.update({
    "scePthreadCondInit": ("2Tb92quprl0", "libkernel", 1, "B"),
    "scePthreadCondDestroy": ("g+PZd2hiacg", "libkernel", 1, "B"),
    "scePthreadCondWait": ("WKAXJ4XBPQ4", "libkernel", 1, "B"),
    "scePthreadCondSignal": ("kDh-NfxgMtE", "libkernel", 1, "B"),
    "scePthreadCondBroadcast": ("JGgj7Uvrl+A", "libkernel", 1, "B"),
    "scePthreadCondattrInit": ("m5-2bsNfv7s", "libkernel", 1, "B"),
    "scePthreadCondattrDestroy": ("waPcxYiR3WA", "libkernel", 1, "B"),
    "posix_mutex_lock": ("7H0iTOciTLo", "libkernel", 1, "B"),
    "posix_mutex_unlock": ("2Z+PpY6CaJg", "libkernel", 1, "B"),
    "scePthreadMutexTimedlock": ("IafI2PxcPnQ", "libkernel", 1, "B"),
    "scePthreadCondattrSetclock": ("c-bxj027czs", "libkernel", 1, "B"),
    "posix_condattr_setclock": ("EjllaAqAPZo", "libkernel", 1, "B"),
    "posix_cond_timedwait": ("27bAgiJmOh0", "libkernel", 1, "B"),
})
fixture.EXPORTS = {"_start": 2, "KernelMutexSemanticsReceipt": 1}

if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: BuildKernelMutexSemanticsFixture.py linked.elf output.elf")
    fixture.package(*sys.argv[1:])
