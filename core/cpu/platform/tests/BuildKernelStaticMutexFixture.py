"""Reuse the genuine linked packager; scope exact CPU12 imports as libc 0/1."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("kernel_static_linker", Path(__file__).with_name("BuildKernelMutexThreadsFixture.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
fixture.IMPORTS = {
    name: fixture.IMPORTS[name] for name in (
        "scePthreadMutexInit", "scePthreadMutexDestroy", "scePthreadMutexTrylock",
        "thread_create", "thread_join", "thread_yield", "thread_self", "process_exit")
}
fixture.IMPORTS.update({
    "posix_mutex_lock": ("7H0iTOciTLo", "libkernel", 1, "B"),
    "posix_mutex_unlock": ("2Z+PpY6CaJg", "libkernel", 1, "B"),
    "posix_cond_wait": ("Op8TBGY5KHg", "libkernel", 1, "B"),
    "posix_cond_broadcast": ("mkx2fVhNMsg", "libkernel", 1, "B"),
    "scePthreadCondDestroy": ("g+PZd2hiacg", "libkernel", 1, "B"),
})
fixture.EXPORTS = {"_start": 2, "KernelStaticMutexReceipt": 1}

if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: BuildKernelStaticMutexFixture.py linked.elf output.elf")
    fixture.package(*sys.argv[1:])
    output = Path(sys.argv[2])
    image = bytearray(output.read_bytes())
    phoff = struct.unpack_from("<Q", image, 32)[0]
    phnum = struct.unpack_from("<H", image, 56)[0]
    headers = [struct.unpack_from("<IIQQQQQQ", image, phoff + i*56) for i in range(phnum)]
    dynamic = next(h for h in headers if h[0] == 2)
    data = next(h for h in headers if h[0] == 0x61000000)
    tags = list(struct.iter_unpack("<QQ", image[dynamic[2]:dynamic[2]+dynamic[5]]))
    lookup = dict(tags)
    start = data[2] + lookup[0x61000035]
    end = start + lookup[0x61000037]
    strings = bytearray(image[start:end])
    position = 0
    rewritten = 0
    while position < len(strings):
        boundary = strings.index(0, position)
        value = strings[position:boundary]
        if value.endswith(b"#B#B"):
            strings[position:boundary] = value[:-4] + b"#A#B"
            rewritten += 1
        position = boundary + 1
    if rewritten != len(fixture.IMPORTS):
        raise ValueError("Exact linked libc import scopes missing")
    image[start:end] = strings
    for index, (tag, value) in enumerate(tags):
        if tag == 0x61000049:  # library import ID 0; module import retains ID 1
            struct.pack_into("<QQ", image, dynamic[2] + index*16, tag, value & ((1 << 48)-1))
    output.write_bytes(image)
    receipt_path = output.with_suffix(".receipt.json")
    receipt = json.loads(receipt_path.read_text())
    receipt["sce_sha256"] = hashlib.sha256(image).hexdigest()
    for row in receipt["imports"]:
        row["module_id"] = 1
        row["library_id"] = 0
    receipt["admission"] = "Public synthetic libc 0/1 linked static mutex/condition fixture; source-backed engineering inference, vendorunknown; no vendor/title execution"
    receipt_path.write_text(json.dumps(receipt, indent=2)+"\n")
