"""Package true linked timed PLTs in each observed consumer's exact scope."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("timeout_linker", Path(__file__).with_name("BuildKernelMutexThreadsFixture.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
fixture.IMPORTS = {name: fixture.IMPORTS[name] for name in (
    "scePthreadMutexInit", "scePthreadMutexLock", "scePthreadMutexUnlock", "scePthreadMutexTrylock",
    "scePthreadMutexDestroy", "scePthreadMutexattrInit", "scePthreadMutexattrSettype", "scePthreadMutexattrDestroy",
    "thread_create", "thread_join", "thread_yield", "thread_self", "process_exit")}
fixture.IMPORTS.update({
    "scePthreadCondInit": ("2Tb92quprl0", "libkernel", 1, "B"),
    "scePthreadCondSignal": ("kDh-NfxgMtE", "libkernel", 1, "B"),
    "scePthreadCondDestroy": ("g+PZd2hiacg", "libkernel", 1, "B"),
})
fixture.EXPORTS = {"_start": 2, "KernelConditionTimeoutReceipt": 1}
if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[3] not in ("posix", "relative"):
        raise SystemExit("Usage: BuildKernelConditionTimeoutFixture.py linked.elf output.elf posix|relative")
    relative = sys.argv[3] == "relative"
    timed_nid = "BmMjYxmew1w" if relative else "27bAgiJmOh0"
    fixture.IMPORTS["timed_wait"] = (timed_nid, "libkernel" if relative else "libScePosix", 1, "B")
    fixture.package(*sys.argv[1:3])
    output = Path(sys.argv[2]); image = bytearray(output.read_bytes())
    phoff = struct.unpack_from("<Q", image, 32)[0]; phnum = struct.unpack_from("<H", image, 56)[0]
    headers = [struct.unpack_from("<IIQQQQQQ", image, phoff+i*56) for i in range(phnum)]
    di,dynamic = next((i,h) for i,h in enumerate(headers) if h[0] == 2)
    ti,data = next((i,h) for i,h in enumerate(headers) if h[0] == 0x61000000)
    tags = list(struct.iter_unpack("<QQ", image[dynamic[2]:dynamic[2]+dynamic[5]])); lookup = dict(tags)
    strings = bytearray(image[data[2]+lookup[0x61000035]:data[2]+lookup[0x61000035]+lookup[0x61000037]])
    position = changed = 0
    while position < len(strings):
        end = strings.index(0, position); value = strings[position:end].decode("ascii")
        if value.endswith("#B#B"):
            value = value[:-4] + ("#A#B" if relative else ("#r#Y" if value[:-4] == timed_nid else "#s#Y"))
            strings[position:end] = value.encode("ascii"); changed += 1
        position = end+1
    if changed != len(fixture.IMPORTS): raise ValueError("Missing true linked import scopes")
    posix_name = len(strings)
    if not relative: strings.extend(b"libScePosix\0")
    string_offset = len(image)-data[2]; image.extend(strings)
    rewritten = []
    for tag,value in tags:
        if not tag: continue
        if tag == 0x61000035: value = string_offset
        if tag == 0x61000037: value = len(strings)
        if tag == 0x61000045: value = ((1 if relative else 24)<<48)|(value & ((1<<48)-1))
        if tag == 0x61000049: value = ((0 if relative else 44)<<48)|(value & ((1<<48)-1))
        rewritten.append((tag,value))
    if not relative: rewritten.append((0x61000049,(43<<48)|(1<<32)|posix_name))
    rewritten.append((0,0)); dynamic_offset = len(image)
    dynamics = b"".join(struct.pack("<QQ",*row) for row in rewritten); image.extend(dynamics)
    struct.pack_into("<IIQQQQQQ",image,phoff+di*56,2,4,dynamic_offset,0,0,len(dynamics),len(dynamics),8)
    struct.pack_into("<IIQQQQQQ",image,phoff+ti*56,*((*data[:5],dynamic_offset-data[2],0,data[7])))
    output.write_bytes(image)
    receipt_path = output.with_suffix(".receipt.json"); receipt = json.loads(receipt_path.read_text())
    receipt["sce_sha256"] = hashlib.sha256(image).hexdigest()
    for row in receipt["imports"]:
        row["library"] = fixture.IMPORTS[row["name"]][1]; row["module"] = "libkernel"
        row["library_id"] = 0 if relative else (43 if row["library"] == "libScePosix" else 44)
        row["module_id"] = 1 if relative else 24
    receipt["admission"] = "Public synthetic linked timed-condition fixture; source engineering inference vendorunknown; no title execution"
    receipt_path.write_text(json.dumps(receipt,indent=2)+"\n")
