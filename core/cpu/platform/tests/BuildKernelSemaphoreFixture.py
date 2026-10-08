"""Preserve genuine linked FUNC/PLT imports; select the engineering fixture only."""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
path = Path(__file__).with_name("BuildKernelMutexThreadsFixture.py")
spec = importlib.util.spec_from_file_location("kernel_semaphore_packager", path)
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
fixture.IMPORTS = {
    "sema_create": ("188x57JYp0g", "libkernel", 1, "B"),
    "sema_delete": ("R1Jvn8bSCW8", "libkernel", 1, "B"),
    "sema_wait": ("Zxa0VhQVTsk", "libkernel", 1, "B"),
    "sema_signal": ("4czppHBiriw", "libkernel", 1, "B"),
    "thread_create": ("6UgtwV+0zb4", "libkernel", 1, "B"),
    "thread_yield": ("T72hz6ffq08", "libkernel", 1, "B"),
    "thread_join": ("onNY9Byn-W8", "libkernel", 1, "B"),
    "thread_self": ("aI+OeCz8xrQ", "libkernel", 1, "B"),
    "process_exit": ("6Z83sYWFlA8", "libkernel", 1, "B"),
    "attr_init": ("nsYoNRywwNg", "libkernel", 1, "B"),
    "attr_destroy": ("62KCwEMmzcM", "libkernel", 1, "B"),
    "attr_priority": ("DzES9hQF4f4", "libkernel", 1, "B"),
    "attr_inherit": ("eXbUSpEaTsA", "libkernel", 1, "B"),
    "mutex_init": ("cmo1RIYva9o", "libkernel", 1, "B"),
    "mutex_lock": ("9UK1vLZQft4", "libkernel", 1, "B"),
    "mutex_unlock": ("tn3VlD0hG60", "libkernel", 1, "B"),
    "mutex_destroy": ("2Of0f+3mhhE", "libkernel", 1, "B"),
    "mutex_attr_init": ("F8bUHwAG284", "libkernel", 1, "B"),
    "mutex_attr_protocol": ("1FGvU0i9saQ", "libkernel", 1, "B"),
    "mutex_attr_destroy": ("smWEktiyyG0", "libkernel", 1, "B"),
}
fixture.EXPORTS = {"_start": 2, "KernelSemaphoreReceipt": 1}
if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: BuildKernelSemaphoreFixture.py linked.elf output.elf")
    fixture.package(*sys.argv[1:])
