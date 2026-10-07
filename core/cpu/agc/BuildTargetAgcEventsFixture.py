"""Retain public linker's genuine SCE PLT, TLS and relocations for AGC events."""
import importlib.util
from pathlib import Path
import sys
sys.dont_write_bytecode = True
shared = Path(__file__).resolve().parents[1] / "platform/tests/BuildKernelEventsFixture.py"
spec = importlib.util.spec_from_file_location("agc_events_link_packager", shared)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
# Reuse the existing linked ELF validator, including the derived two-scope table.
# No input instructions, relocations or linked STT_FUNC types are manufactured.
module.fixture.IMPORTS = {
    "queue_create": ("D0OdFMjp46I", "libkernel", 1, "B"),
    "queue_delete": ("jpFjmgAC5AE", "libkernel", 1, "B"),
    "queue_wait": ("fzyMKs9kim0", "libkernel", 1, "B"),
    "graphics_add": ("w2rJhmD+dsE", "libSceAgcDriver", 2, "C"),
    "graphics_delete": ("DL2RXaXOy88", "libSceAgcDriver", 2, "C"),
    "thread_yield": ("T72hz6ffq08", "libkernel", 1, "B"),
    "process_exit": ("6Z83sYWFlA8", "libkernel", 1, "B"),
}
module.fixture.EXPORTS = {"_start": 2, "AgcEventsReceipt": 1}
if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: BuildTargetAgcEventsFixture.py linked.elf output.elf")
    module.fixture.package(*sys.argv[1:])
