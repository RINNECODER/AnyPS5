"""Reuse genuine linked SCE packaging for the public RR scheduler fixture."""
import importlib.util
from pathlib import Path
import sys
sys.dont_write_bytecode = True
path = Path(__file__).with_name("BuildKernelPriorityFixture.py")
spec = importlib.util.spec_from_file_location("kernel_priority_fixture", path)
priority = importlib.util.module_from_spec(spec)
spec.loader.exec_module(priority)
fixture = priority.fixture
fixture.IMPORTS.pop("scePthreadMutexTrylock")
fixture.IMPORTS.pop("scePthreadMutexattrSettype")
fixture.EXPORTS = {"_start": 2, "KernelRoundRobinReceipt": 1}
if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: BuildKernelRoundRobinFixture.py linked.elf output.elf")
    fixture.package(*sys.argv[1:])
