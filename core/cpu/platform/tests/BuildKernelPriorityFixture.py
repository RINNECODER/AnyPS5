"""Reuse the genuine linker/SCE packager, with explicit priority-attribute imports."""
import importlib.util
from pathlib import Path
import sys
sys.dont_write_bytecode = True
path = Path(__file__).with_name("BuildKernelMutexThreadsFixture.py")
spec = importlib.util.spec_from_file_location("kernel_mutex_fixture", path)
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
fixture.IMPORTS.update({
    "attr_init": ("nsYoNRywwNg", "libkernel", 1, "B"),
    "attr_destroy": ("62KCwEMmzcM", "libkernel", 1, "B"),
    "attr_priority": ("DzES9hQF4f4", "libkernel", 1, "B"),
    "attr_getpriority": ("FXPWHNk8Of0", "libkernel", 1, "B"),
    "attr_inherit": ("eXbUSpEaTsA", "libkernel", 1, "B"),
    "attr_policy": ("4+h9EzwKF4I", "libkernel", 1, "B"),
})
fixture.EXPORTS = {"_start": 2, "KernelPriorityReceipt": 1}
if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: BuildKernelPriorityFixture.py linked.elf output.elf")
    fixture.package(*sys.argv[1:])
