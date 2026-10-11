"""Package the public libkernel sync fixture; preserve genuine linked FUNC/PLT imports."""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
path = Path(__file__).with_name("BuildKernelMutexThreadsFixture.py")
spec = importlib.util.spec_from_file_location("kernel_sync_packager", path)
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
fixture.IMPORTS = {
    "sema_create": ("188x57JYp0g", "libkernel", 1, "B"),
    "sema_delete": ("R1Jvn8bSCW8", "libkernel", 1, "B"),
    "sema_wait": ("Zxa0VhQVTsk", "libkernel", 1, "B"),
    "sema_signal": ("4czppHBiriw", "libkernel", 1, "B"),
    "sema_poll": ("12wOHk8ywb0", "libkernel", 1, "B"),
    "sema_cancel": ("4DM06U2BNEY", "libkernel", 1, "B"),
    "evf_create": ("BpFoboUJoZU", "libkernel", 1, "B"),
    "evf_delete": ("8mql9OcQnd4", "libkernel", 1, "B"),
    "evf_set": ("IOnSvHzqu6A", "libkernel", 1, "B"),
    "evf_clear": ("7uhBFWRAS60", "libkernel", 1, "B"),
    "evf_wait": ("JTvBflhYazQ", "libkernel", 1, "B"),
    "evf_poll": ("9lvj5DjHZiA", "libkernel", 1, "B"),
    "evf_cancel": ("PZku4ZrXJqg", "libkernel", 1, "B"),
    "queue_create": ("D0OdFMjp46I", "libkernel", 1, "B"),
    "queue_delete": ("jpFjmgAC5AE", "libkernel", 1, "B"),
    "queue_wait": ("fzyMKs9kim0", "libkernel", 1, "B"),
    "user_add": ("4R6-OvI2cEA", "libkernel", 1, "B"),
    "user_add_edge": ("WDszmSbWuDk", "libkernel", 1, "B"),
    "user_trigger": ("F6e0kwo4cnk", "libkernel", 1, "B"),
    "user_delete": ("LJDwdSNTnDg", "libkernel", 1, "B"),
    "timer_add": ("57ZK+ODEXWY", "libkernel", 1, "B"),
    "timer_delete": ("YWQFUyXIVdU", "libkernel", 1, "B"),
    "hrtimer_add": ("R74tt43xP6k", "libkernel", 1, "B"),
    "hrtimer_delete": ("J+LF6LwObXU", "libkernel", 1, "B"),
    "event_id": ("mJ7aghmgvfc", "libkernel", 1, "B"),
    "event_filter": ("23CPPI1tyBY", "libkernel", 1, "B"),
    "event_data": ("kwGyyjohI50", "libkernel", 1, "B"),
    "event_udata": ("vz+pg2zdopI", "libkernel", 1, "B"),
    "event_fflags": ("Q0qr9AyqJSk", "libkernel", 1, "B"),
    "thread_create": ("6UgtwV+0zb4", "libkernel", 1, "B"),
    "thread_yield": ("T72hz6ffq08", "libkernel", 1, "B"),
    "thread_join": ("onNY9Byn-W8", "libkernel", 1, "B"),
    "thread_self": ("aI+OeCz8xrQ", "libkernel", 1, "B"),
    "process_exit": ("6Z83sYWFlA8", "libkernel", 1, "B"),
}
fixture.EXPORTS = {"_start": 2, "KernelSyncReceipt": 1}
if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: BuildKernelSyncFixture.py linked.elf output.elf")
    fixture.package(*sys.argv[1:])
