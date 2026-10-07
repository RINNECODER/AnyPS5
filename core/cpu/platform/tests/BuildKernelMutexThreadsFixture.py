"""Package the genuine linked public mutex/thread fixture; preserve all symbol types."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
helper = Path(__file__).resolve().parents[2] / "tests/BuildSceCrtFixture.py"
spec = importlib.util.spec_from_file_location("sce_crt_fixture_helper", helper)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
Elf, nid = module.Elf, module.nid
IMPORTS = {
    "scePthreadMutexInit": ("cmo1RIYva9o", "libkernel", 1, "B"),
    "scePthreadMutexTrylock": ("upoVrzMHFeE", "libkernel", 1, "B"),
    "scePthreadMutexUnlock": ("tn3VlD0hG60", "libkernel", 1, "B"),
    "scePthreadMutexDestroy": ("2Of0f+3mhhE", "libkernel", 1, "B"),
    "scePthreadMutexattrInit": ("F8bUHwAG284", "libkernel", 1, "B"),
    "scePthreadMutexattrSettype": ("iMp8QpE+XO4", "libkernel", 1, "B"),
    "scePthreadMutexattrDestroy": ("smWEktiyyG0", "libkernel", 1, "B"),
    "scePthreadMutexattrSetprotocol": ("1FGvU0i9saQ", "libkernel", 1, "B"),
    "scePthreadMutexLock": ("9UK1vLZQft4", "libkernel", 1, "B"),
    "thread_create": ("6UgtwV+0zb4", "libkernel", 1, "B"),
    "thread_yield": ("T72hz6ffq08", "libkernel", 1, "B"),
    "thread_join": ("onNY9Byn-W8", "libkernel", 1, "B"),
    "thread_self": ("aI+OeCz8xrQ", "libkernel", 1, "B"),
    "process_exit": ("6Z83sYWFlA8", "libkernel", 1, "B"),
}
EXPORTS = {"_start": 2, "KernelMutexReceipt": 1}


def package(linked, output):
    elf = Elf(Path(linked).read_bytes())
    if struct.unpack_from("<H", elf.image, 16)[0] != 3:
        raise ValueError("Platform fixture needs genuine linker ET_DYN")
    if elf.needed:
        raise ValueError("Platform fixture must have no synthetic dependency providers")
    if 16 in elf.tags or elf.tags.get(30, 0) & 2:
        raise ValueError("Platform fixture requires default symbol binding; DT_SYMBOLIC/DF_SYMBOLIC are not admitted")
    tls, = [header for header in elf.headers if header[0] == 7]
    if not tls[5] or tls[6] <= tls[5] or tls[7] < 16:
        raise ValueError("Mutex fixture requires genuine initialized and zero-fill TLS")
    image = bytearray(elf.image)
    shoff = struct.unpack_from("<Q", image, 40)[0]
    shsize, shcount = struct.unpack_from("<HH", image, 58)
    if shsize != 64:
        raise ValueError("Invalid linked section table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", image, shoff+i*64) for i in range(shcount)]
    dynsym, = [section for section in sections if section[1] == 11]
    if dynsym[9] != 24 or dynsym[5] % 24:
        raise ValueError("Invalid linked dynamic symbol table")
    symbols = bytearray(elf.slice(dynsym[4], dynsym[5]))
    old_strings = elf.virtual(elf.tags[5], elf.tags[10])
    strings = bytearray(b"\0")

    def old_string(offset):
        end = old_strings.find(b"\0", offset)
        if offset >= len(old_strings) or end < 0:
            raise ValueError("Invalid linked string")
        return old_strings[offset:end].decode("ascii")

    def string(value):
        offset = len(strings)
        strings.extend(value.encode("ascii") + b"\0")
        return offset

    found, definitions, entry = set(), set(), None
    for offset in range(24, len(symbols), 24):
        name_offset, info, visibility, section, value, size = struct.unpack_from("<IBBHQQ", symbols, offset)
        name = old_string(name_offset)
        if section:
            if name not in EXPORTS or info != (0x10 | EXPORTS[name]) or visibility != 3 or name in definitions:
                raise ValueError(f"Unexpected fixture definition {name}")
            definitions.add(name)
            if name == "_start":
                entry = value
            scoped = nid(name) + "#A#A"
        else:
            if name not in IMPORTS or info != 0x12 or visibility != 0 or value or size:
                raise ValueError(f"Import must already be genuine undefined STB_GLOBAL/STT_FUNC: {name}")
            found.add(name)
            import_nid, _, _, suffix = IMPORTS[name]
            scoped = import_nid + "#" + suffix + "#" + suffix
        struct.pack_into("<I", symbols, offset, string(scoped))
    if definitions != set(EXPORTS) or found != set(IMPORTS) or entry != struct.unpack_from("<Q", image, 24)[0]:
        raise ValueError("Missing genuine function imports or _start entry")
    own = string("KernelMutexThreadsFixture")
    tags = [(0x61000043, (0x101 << 32)|own), (0x61000047,(1 << 32)|own), (0x61000017,0)]
    for name, identity in (("libkernel",1),):
        offset = string(name)
        tags += [(0x61000045,(identity << 48)|(0x101 << 32)|offset),
                 (0x61000049,(identity << 48)|(1 << 32)|offset)]
    data = bytearray()

    def append(table):
        data.extend(b"\0" * (-len(data)%8))
        offset = len(data)
        data.extend(table)
        return offset

    tags += [(0x61000035,append(strings)), (0x61000037,len(strings)),
             (0x61000039,append(symbols)), (0x6100003b,24), (0x6100003f,len(symbols))]
    rela = elf.virtual(elf.tags[7], elf.tags[8])
    plt = elf.virtual(elf.tags[23], elf.tags[2])
    if not rela or not plt or len(rela)%24 or len(plt)%24 or elf.tags[9] != 24 or elf.tags[20] != 7:
        raise ValueError("Fixture requires genuine ELF64 RELATIVE and PLT relocations")
    relative_types = {info & 0xffffffff for _,info,_ in struct.iter_unpack("<QQq",rela)}
    plt_types = {info & 0xffffffff for _,info,_ in struct.iter_unpack("<QQq",plt)}
    if relative_types != {8} or plt_types != {7} or len(plt) != len(IMPORTS)*24:
        raise ValueError("Unexpected linker relocation contracts")
    tags += [(0x6100002f,append(rela)), (0x61000031,len(rela)), (0x61000033,24),
             (0x61000029,append(plt)), (0x6100002d,len(plt)), (0x6100002b,7)]
    if 3 in elf.tags:
        tags.append((0x61000027,elf.tags[3]))
    replaced = {2,3,4,5,6,7,8,9,10,11,20,23,0x6ffffef5}
    allowed = {16,21,30,0x6ffffff9,0x6ffffffb}
    for tag,value in elf.tags.items():
        if tag in replaced:
            continue
        if tag not in allowed:
            raise ValueError(f"Unadmitted linked metadata {tag:#x}")
        tags.append((tag,value))
    tags.append((0,0))
    image.extend(b"\0" * (-len(image)%8))
    data_offset = len(image)
    image.extend(data)
    dynamic_offset = len(image)
    dynamics = b"".join(struct.pack("<QQ",*tag) for tag in tags)
    image.extend(dynamics)
    headers = [header for header in elf.headers if header[0] in (1,7,0x6474e551,0x6474e552)]
    headers += [(2,4,dynamic_offset,0,0,len(dynamics),len(dynamics),8),
                (0x61000000,4,data_offset,0,0,len(data),0,8)]
    phoff = len(image)
    image.extend(b"".join(struct.pack("<IIQQQQQQ",*header) for header in headers))
    image[7:9] = bytes((9,2))
    struct.pack_into("<H",image,16,0xfe10)
    struct.pack_into("<QQ",image,32,phoff,0)
    struct.pack_into("<HHHH",image,56,len(headers),0,0,0)
    destination = Path(output)
    destination.write_bytes(image)
    receipt = {"schema_version":1,"linked_sha256":hashlib.sha256(elf.image).hexdigest(),
               "sce_sha256":hashlib.sha256(image).hexdigest(),"entry":entry,
               "imports":[{"name":name,"nid":row[0],"library":row[1],"module":row[1],
                           "library_version":1,"module_major":1,"module_minor":1,
                           "symbol_type":2,"type_origin":"validated linked STT_FUNC; unchanged"}
                          for name,row in IMPORTS.items()],
               "relocations":{"relative":len(rela)//24,"plt":len(plt)//24},
               "admission":"Public synthetic kernel scheduler fixture; no vendor semantics proof"}
    destination.with_suffix(".receipt.json").write_text(json.dumps(receipt,indent=2)+"\n")
    print(json.dumps(receipt,separators=(",",":")))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: BuildKernelMutexThreadsFixture.py linked.elf output.elf")
    package(*sys.argv[1:])
