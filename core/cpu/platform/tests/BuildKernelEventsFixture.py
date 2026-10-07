"""Preserve genuine x86 linker symbols, TLS and SCE PLT in the public queue oracle."""
import importlib.util
from pathlib import Path
import sys
sys.dont_write_bytecode = True
path = Path(__file__).with_name("BuildKernelMutexThreadsFixture.py")
spec = importlib.util.spec_from_file_location("kernel_events_packager", path)
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
# This fixture also links one genuine libSceAgcDriver source import. Preserve
# the shared packager while deriving its host scope table from these imports.
source = path.read_text()
scope = 'for name, identity in (("libkernel",1),):'
if source.count(scope) != 1:
    raise ValueError("Shared fixture host-scope table changed")
exec(compile(source.replace(scope,
    'for name, identity in sorted({(row[1], row[2]) for row in IMPORTS.values()}):'),
    str(path), "exec"), fixture.__dict__)
fixture.IMPORTS = {
    "queue_create": ("D0OdFMjp46I", "libkernel", 1, "B"),
    "queue_delete": ("jpFjmgAC5AE", "libkernel", 1, "B"),
    "graphics_delete": ("DL2RXaXOy88", "libSceAgcDriver", 2, "C"),
    "queue_wait": ("fzyMKs9kim0", "libkernel", 1, "B"),
    "thread_create": ("6UgtwV+0zb4", "libkernel", 1, "B"),
    "thread_yield": ("T72hz6ffq08", "libkernel", 1, "B"),
    "thread_join": ("onNY9Byn-W8", "libkernel", 1, "B"),
    "thread_self": ("aI+OeCz8xrQ", "libkernel", 1, "B"),
    "process_exit": ("6Z83sYWFlA8", "libkernel", 1, "B"),
}
fixture.EXPORTS = {"_start": 2, "KernelEventsReceipt": 1}
if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: BuildKernelEventsFixture.py linked.elf output.elf")
    fixture.package(*sys.argv[1:])
