from pathlib import Path
import struct
import sys


def package(linked_path, output_path):
    image = bytearray(Path(linked_path).read_bytes())
    if image[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("Fixture packaging requires a linker-produced little-endian ELF64")
    if struct.unpack_from("<HH", image, 16) != (3, 62):
        raise ValueError("Fixture packaging requires a linked x86-64 ET_DYN image")
    phoff, = struct.unpack_from("<Q", image, 32)
    phsize, phcount = struct.unpack_from("<HH", image, 54)
    shoff, = struct.unpack_from("<Q", image, 40)
    shsize, shcount = struct.unpack_from("<HH", image, 58)
    if phsize != 56 or shsize != 64:
        raise ValueError("Unsupported linker header layout")
    headers = [struct.unpack_from("<IIQQQQQQ", image, phoff + i * phsize) for i in range(phcount)]
    sections = [struct.unpack_from("<IIQQQQIIQQ", image, shoff + i * shsize) for i in range(shcount)]

    def slice_bytes(offset, size):
        if offset > len(image) or size > len(image) - offset:
            raise ValueError("Linked fixture table exceeds file")
        return bytes(image[offset:offset + size])

    def virtual(address, size):
        for header in headers:
            if header[0] == 1 and address >= header[3] and address - header[3] + size <= header[5]:
                return slice_bytes(header[2] + address - header[3], size)
        raise ValueError("Linked fixture table is not file-backed")

    dynamic = next(header for header in headers if header[0] == 2)
    tags = {}
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack_from("<QQ", image, offset)
        if tag == 0:
            break
        if tag == 1 or tag in (12, 13, 25, 26, 32):
            raise ValueError("Fixture must not depend on host libraries or module initialization")
        if tag in tags:
            raise ValueError("Duplicate linker dynamic tag")
        tags[tag] = value
    dynsym = next(section for section in sections if section[1] == 11)
    if dynsym[9] != 24 or dynsym[5] % 24:
        raise ValueError("Invalid linked dynamic symbol table")
    symbols = bytearray(slice_bytes(dynsym[4], dynsym[5]))
    old_strings = virtual(tags[5], tags[10])
    strings = bytearray(b"\0")
    libc = {"Q3VBxCXhUHs", "+P6FRGH4LfA", "8zTFvBIAIN8", "j4ViWNHEgww", "Ovb2dSJOAuE", "uMei1W9uyNo"}
    libkernel = {"1G3lF1Gg1k8", "Cg4srZ6TKbU", "+r3rMFwItV4", "oib76F-12fk", "UK2Tl2DWUns",
                 "pO96TwzOm5E", "C0f7TJcbfac", "rTXw65xmLIA", "B+vc2AO2Zrc", "L-Q3LEjIbgA",
                 "IWIBBdTHit4", "7oxv3PPCumo", "vSMAm3cxYTY", "rVjRvHJ0X6c", "cQke9UuBQOk", "MBuItvba6z8"}
    user_service = {"j3YMu1MVNNo", "CdWp0oHWGr0", "fPhymKNvK-A", "1xxcMiGu2fo"}
    system_service = {"fZo48un7LK4", "SsC-m-S9JTA", "Vo5V8KAwCmk"}
    required = libc | libkernel | user_service | system_service
    found = set()
    for offset in range(0, len(symbols), 24):
        name_offset, info, other, section, value, size = struct.unpack_from("<IBBHQQ", symbols, offset)
        name = old_strings[name_offset:old_strings.index(0, name_offset)].decode("ascii")
        if offset and section == 0 and info >> 4:
            if name not in required or (info & 15) not in (0, 2):
                raise ValueError(f"Unexpected compiled guest import: {name}")
            found.add(name)
            name += ("#B#B" if name in libc else "#C#C" if name in libkernel else
                     "#D#D" if name in user_service else "#E#E")
            symbols[offset + 4] = (info & 0xf0) | 2
        if offset == 0:
            if name or any(symbols[:24]):
                raise ValueError("Invalid linked null symbol")
            continue
        struct.pack_into("<I", symbols, offset, len(strings))
        strings += name.encode("ascii") + b"\0"
    if found != required:
        raise ValueError(f"Compiled fixture did not retain all native service imports: {required - found}")
    libc_offset = len(strings)
    strings += b"libc\0"
    kernel_offset = len(strings)
    strings += b"libkernel\0"
    user_offset = len(strings)
    strings += b"libSceUserService\0"
    system_offset = len(strings)
    strings += b"libSceSystemService\0"
    data = bytearray()

    def append(table):
        data.extend(b"\0" * (-len(data) % 8))
        offset = len(data)
        data.extend(table)
        return offset

    str_offset = append(strings)
    sym_offset = append(symbols)
    rela = virtual(tags[7], tags[8]) if tags.get(8) else b""
    plt = virtual(tags[23], tags[2]) if tags.get(2) else b""
    if not rela or not plt or len(rela) % 24 or len(plt) % 24:
        raise ValueError("Compiled fixture must contain genuine RELATIVE and PLT relocation tables")
    if tags.get(9) != 24 or tags.get(20) != 7:
        raise ValueError("Linked fixture requires ELF64 RELA entries")
    if not any(struct.unpack_from("<Q", rela, i + 8)[0] & 0xffffffff == 8 for i in range(0, len(rela), 24)):
        raise ValueError("Compiled fixture did not retain position-independent pointer relocations")
    rela_offset = append(rela)
    plt_offset = append(plt)
    library = (1 << 48) | (1 << 32) | libc_offset
    module = (1 << 48) | (1 << 40) | (1 << 32) | libc_offset
    kernel_library = (2 << 48) | (1 << 32) | kernel_offset
    kernel_module = (2 << 48) | (1 << 40) | (1 << 32) | kernel_offset
    user_library = (3 << 48) | (1 << 32) | user_offset
    user_module = (3 << 48) | (1 << 40) | (1 << 32) | user_offset
    system_library = (4 << 48) | (1 << 32) | system_offset
    system_module = (4 << 48) | (1 << 40) | (1 << 32) | system_offset
    sce_tags = [(0x61000049, library), (0x61000045, module),
                (0x61000049, kernel_library), (0x61000045, kernel_module),
                (0x61000049, user_library), (0x61000045, user_module),
                (0x61000049, system_library), (0x61000045, system_module),
                (0x61000035, str_offset), (0x61000037, len(strings)),
                (0x61000039, sym_offset), (0x6100003b, 24), (0x6100003f, len(symbols)),
                (0x6100002f, rela_offset), (0x61000031, len(rela)), (0x61000033, 24),
                (0x61000029, plt_offset), (0x6100002d, len(plt)), (0x6100002b, 7)]
    if 3 in tags:
        sce_tags.append((0x61000027, tags[3]))
    sce_tags.append((0, 0))
    image.extend(b"\0" * (-len(image) % 8))
    data_offset = len(image)
    image.extend(data)
    image.extend(b"\0" * (-len(image) % 8))
    dynamic_offset = len(image)
    dynamic_data = b"".join(struct.pack("<QQ", *tag) for tag in sce_tags)
    image.extend(dynamic_data)
    kept = [header for header in headers if header[0] in (1, 0x6474e551)]
    kept += [(2, 4, dynamic_offset, 0, 0, len(dynamic_data), len(dynamic_data), 8),
             (0x61000000, 4, data_offset, 0, 0, len(data), 0, 8)]
    image.extend(b"\0" * (-len(image) % 8))
    table_offset = len(image)
    image.extend(b"".join(struct.pack("<IIQQQQQQ", *header) for header in kept))
    image[7:9] = bytes((9, 2))
    struct.pack_into("<H", image, 16, 0xfe10)
    struct.pack_into("<Q", image, 32, table_offset)
    struct.pack_into("<Q", image, 40, 0)
    struct.pack_into("<H", image, 56, len(kept))
    struct.pack_into("<HHH", image, 58, 0, 0, 0)
    Path(output_path).write_bytes(image)
    print(f"compiled SCE fixture: {len(image)} bytes, {len(found)} scoped imports, "
          f"{len(rela) // 24 + len(plt) // 24} genuine linker relocations")


if __name__ == "__main__":
    package(*sys.argv[1:])
