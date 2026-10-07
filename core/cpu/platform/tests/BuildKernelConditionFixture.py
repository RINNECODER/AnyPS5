"""Use the genuine linked mutex/SCE packager with qualified condition imports."""
import hashlib
import json
import struct
import importlib.util
from pathlib import Path
import sys
sys.dont_write_bytecode = True
path = Path(__file__).with_name("BuildKernelMutexThreadsFixture.py")
spec = importlib.util.spec_from_file_location("kernel_condition_linker", path)
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
fixture.IMPORTS.update({
    "scePthreadCondInit": ("2Tb92quprl0", "libkernel", 1, "B"),
    "scePthreadCondDestroy": ("g+PZd2hiacg", "libkernel", 1, "B"),
    "scePthreadCondWait": ("WKAXJ4XBPQ4", "libkernel", 1, "B"),
    "scePthreadCondSignal": ("kDh-NfxgMtE", "libkernel", 1, "B"),
    "scePthreadCondBroadcast": ("JGgj7Uvrl+A", "libkernel", 1, "B"),
    "scePthreadCondattrInit": ("m5-2bsNfv7s", "libkernel", 1, "B"),
    "scePthreadCondattrDestroy": ("waPcxYiR3WA", "libkernel", 1, "B"),
    "posix_cond_wait": ("Op8TBGY5KHg", "libScePosix", 1, "B"),
    "posix_cond_signal": ("2MOy+rUfuhQ", "libScePosix", 1, "B"),
    "posix_cond_broadcast": ("mkx2fVhNMsg", "libScePosix", 1, "B"),
    "attr_init": ("nsYoNRywwNg", "libkernel", 1, "B"),
    "attr_destroy": ("62KCwEMmzcM", "libkernel", 1, "B"),
    "attr_priority": ("DzES9hQF4f4", "libkernel", 1, "B"),
    "attr_inherit": ("eXbUSpEaTsA", "libkernel", 1, "B"),
    "attr_policy": ("4+h9EzwKF4I", "libkernel", 1, "B"),
})
fixture.EXPORTS = {"_start": 2, "KernelConditionReceipt": 1}
if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: BuildKernelConditionFixture.py linked.elf output.elf")
    fixture.package(*sys.argv[1:])
    # Public synthetic fixture scopes reproduce the pinned eboot module/library
    # IDs. This edits only explicit SCE metadata after validating linked symbols.
    output = Path(sys.argv[2])
    image = bytearray(output.read_bytes())
    phoff = struct.unpack_from("<Q", image, 32)[0]
    phnum = struct.unpack_from("<H", image, 56)[0]
    headers = [struct.unpack_from("<IIQQQQQQ", image, phoff + i*56) for i in range(phnum)]
    dynamic_index, dynamic = next((i,h) for i,h in enumerate(headers) if h[0] == 2)
    data_index, data = next((i,h) for i,h in enumerate(headers) if h[0] == 0x61000000)
    tags = list(struct.iter_unpack("<QQ", image[dynamic[2]:dynamic[2]+dynamic[5]]))
    lookup = dict(tags)
    strings = bytearray(image[data[2]+lookup[0x61000035]:data[2]+lookup[0x61000035]+lookup[0x61000037]])
    posix_nids = {row[0] for row in fixture.IMPORTS.values() if row[1] == "libScePosix"}
    position = 0
    while position < len(strings):
        end = strings.index(0, position)
        value = strings[position:end].decode("ascii")
        if value.endswith("#B#B"):
            value = value[:-4] + ("#r#Y" if value[:-4] in posix_nids else "#s#Y")
            strings[position:end] = value.encode("ascii")
        position = end + 1
    posix_name = len(strings)
    strings.extend(b"libScePosix\0")
    string_offset = len(image) - data[2]
    image.extend(strings)
    rewritten = []
    for tag,value in tags:
        if tag == 0: continue
        if tag == 0x61000035: value = string_offset
        if tag == 0x61000037: value = len(strings)
        if tag == 0x61000045: value = (24 << 48) | (value & ((1 << 48)-1))
        if tag == 0x61000049: value = (44 << 48) | (value & ((1 << 48)-1))
        rewritten.append((tag,value))
    rewritten.extend([(0x61000049,(43<<48)|(1<<32)|posix_name),(0,0)])
    dynamic_offset = len(image)
    dynamics = b"".join(struct.pack("<QQ",*row) for row in rewritten)
    image.extend(dynamics)
    struct.pack_into("<IIQQQQQQ",image,phoff+dynamic_index*56,2,4,dynamic_offset,0,0,len(dynamics),len(dynamics),8)
    struct.pack_into("<IIQQQQQQ",image,phoff+data_index*56,*((*data[:5],dynamic_offset-data[2],0,data[7])))
    output.write_bytes(image)
    receipt_path = output.with_suffix(".receipt.json")
    receipt = json.loads(receipt_path.read_text())
    receipt["sce_sha256"] = hashlib.sha256(image).hexdigest()
    for row in receipt["imports"]:
        row["library"] = fixture.IMPORTS[row["name"]][1]
        row["module"] = "libkernel"
        row["library_id"] = 43 if row["library"] == "libScePosix" else 44
        row["module_id"] = 24
    receipt["admission"] = "Public synthetic linked condition fixture with observed eboot scopes; no vendor/title execution"
    receipt_path.write_text(json.dumps(receipt,indent=2)+"\n")
