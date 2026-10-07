"""Package genuinely compiled guest imports; receipts are independent linker values."""
import hashlib
import json
from pathlib import Path
import struct
import sys
from BuildSceCrtFixture import Elf, nid

FUNCTIONS = ("memcpy", "memset", "strlen", "strcmp", "strncmp", "strncpy", "snprintf", "printf", "strstr",
             "sceLibcMspaceCreate", "sceLibcMspaceDestroy", "sceLibcMspaceMalloc", "sceLibcMspaceFree",
             "sceLibcMspaceRealloc", "sceLibcMspaceMallocStats", "__cxa_atexit", "__cxa_finalize", "_ZdlPv",
             "strncat", "_Stoul", "abort", "__cxa_pure_virtual")


def package(path, kind):
    main, consumer = kind == "main", kind == "consumer"
    elf = Elf(Path(path).read_bytes())
    image = bytearray(elf.image)
    shoff = struct.unpack_from("<Q", image, 40)[0]
    shsize, count = struct.unpack_from("<HH", image, 58)
    if shsize != 64:
        raise ValueError("Invalid compiled Internal section layout")
    sections = [struct.unpack_from("<IIQQQQIIQQ", image, shoff + 64 * index) for index in range(count)]
    dynsym, = [section for section in sections if section[1] == 11]
    symbols = bytearray(elf.slice(dynsym[4], dynsym[5]))
    original = elf.virtual(elf.tags[5], elf.tags[10])
    strings = bytearray(b"\0")
    def old_string(offset):
        end = original.index(0, offset)
        return original[offset:end].decode("ascii")
    def string(value):
        offset = len(strings); strings.extend(value.encode("ascii") + b"\0"); return offset
    definitions = ({"_start": 2, "SceInternalMain": 2, "SceInternalAddresses": 1, "SceInternalOutput": 1, "SceInternalExtraOutput": 1, "SceInternalArenas": 1,
                       "SceInternalStats": 1, "SceInternalTrace": 1, "SceInternalAbortProbe": 2,
                       "SceInternalPureProbe": 2, "SceInternalOwnershipProbe": 2} if main else
                   {**dict.fromkeys(FUNCTIONS, 2), "SceInternalInit": 2, "SceInternalFini": 2,
                    "SceInternalState": 1, "SceInternalPrinted": 1, "SceInternalTwin": 2,
                    "SceInternalExtra": 1, "_Znwm": 2})
    if consumer: definitions = {"SceInternalConsumerInit": 2, "SceInternalConsumerFini": 2, "SceInternalConsumerState": 1}
    imports = {**dict.fromkeys(FUNCTIONS, 2), "SceInternalState": 1, "SceInternalConsumerState": 1,
               "6Z83sYWFlA8": 2, "9BcDykPmo1I": 2, "_Znwm": 2} if main else {"memcpy": 2, "__cxa_atexit": 2, "__cxa_finalize": 2} if consumer else {"9BcDykPmo1I": 2}
    seen_definitions, seen_imports, addresses, locations = set(), set(), {}, {}
    for offset in range(24, len(symbols), 24):
        name_offset, info, visibility, section, value, size = struct.unpack_from("<IBBHQQ", symbols, offset)
        name = old_string(name_offset)
        locations[name] = offset
        if section:
            if definitions.get(name) != info & 15 or visibility != 3:
                raise ValueError(f"Unexpected compiled definition {name}")
            seen_definitions.add(name); addresses[name] = value
            scope = "A"
        else:
            if name not in imports or info & 15 not in (0, imports[name]):
                raise ValueError(f"Unexpected compiled import {name}")
            symbols[offset + 4] = (info & 0xf0) | imports[name]
            seen_imports.add(name)
            scope = "C" if name in ("SceInternalState", "_Znwm") else "E" if name == "SceInternalConsumerState" else "D" if name in ("6Z83sYWFlA8", "9BcDykPmo1I") else "B"
        identity = name if name in ("6Z83sYWFlA8", "9BcDykPmo1I") else nid(name)
        struct.pack_into("<I", symbols, offset, string(identity + "#" + scope + "#" + scope))
    if seen_definitions != set(definitions) or seen_imports != set(imports):
        raise ValueError("Missing genuine compiled Internal imports/exports")
    needed = [old_string(offset) for offset in elf.needed]
    if needed != (["SceInternalConsumer.prx", "SceInternalLibc.prx"] if main else []):
        raise ValueError(f"Unexpected linked dependencies: {needed}")
    if main and any(elf.tags.get(tag) for tag in (12, 13, 25, 26, 27, 28, 32, 33)):
        raise ValueError("Main unexpectedly owns dependency lifecycle")
    tags = []
    tag_locations, name_locations = {}, {}
    def identity(local, module, library, exported):
        module_offset, library_offset = string(module), string(library)
        module_tag, library_tag = (0x61000043, 0x61000047) if exported else (0x61000045, 0x61000049)
        tag_locations[(local, "module")] = len(tags)
        tags.append((module_tag, (local << 48) | (0x101 << 32) | module_offset))
        tag_locations[(local, "library")] = len(tags)
        tags.append((library_tag, (local << 48) | (1 << 32) | library_offset))
        name_locations[(local, "module")] = module_offset
        name_locations[(local, "library")] = library_offset
    own = "SceInternalMain" if main else "SceInternalConsumer" if consumer else "libc"
    identity(0, own, own, True)
    tags.append((0x61000017, 0))
    if main:
        identity(1, "libSceLibcInternal", "libSceLibcInternal", False)
        identity(2, "libc", "libc", False)
        identity(3, "libkernel", "libkernel", False)
        identity(4, "SceInternalConsumer", "SceInternalConsumer", False)
    elif consumer:
        identity(1, "libSceLibcInternal", "libSceLibcInternal", False)
    else:
        identity(3, "libkernel", "libkernel", False)
        # Additional valid library scopes let controls change only memcpy's
        # target identity while retaining initialization and imported state.
        tags += [(0x61000047, (4 << 48) | (1 << 32) | string("other")),
                 (0x61000047, (5 << 48) | (2 << 32) | string("libc"))]
    tags += [(1, string(name)) for name in needed]
    if 14 in elf.tags: tags.append((14, string(old_string(elf.tags[14]))))
    data = bytearray()
    def append(table):
        data.extend(b"\0" * (-len(data) % 8)); offset = len(data); data.extend(table); return offset
    string_offset, symbol_offset = append(strings), append(symbols)
    tags += [(0x61000035, string_offset), (0x61000037, len(strings)),
             (0x61000039, symbol_offset), (0x6100003b, 24), (0x6100003f, len(symbols))]
    for address, size, sce_address, sce_size, sce_entry, entry in (
            (7, 8, 0x6100002f, 0x61000031, 0x61000033, 24),
            (23, 2, 0x61000029, 0x6100002d, 0x6100002b, 7)):
        if elf.tags.get(size):
            tags += [(sce_address, append(elf.virtual(elf.tags[address], elf.tags[size]))),
                     (sce_size, elf.tags[size]), (sce_entry, entry)]
    replaced = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 20, 23}
    allowed = {12, 13, 16, 21, 30, 0x6ffffff9, 0x6ffffffb}
    for tag, value in elf.tags.items():
        if tag in replaced: continue
        if tag not in allowed: raise ValueError(f"Unexpected compiled metadata {tag:#x}")
        tags.append((tag, value))
    if 3 in elf.tags: tags.append((0x61000027, elf.tags[3]))
    tags.append((0, 0))
    image.extend(b"\0" * (-len(image) % 8)); data_offset = len(image); image.extend(data)
    dynamic_offset = len(image); dynamics = b"".join(struct.pack("<QQ", *tag) for tag in tags); image.extend(dynamics)
    headers = [header for header in elf.headers if header[0] in (1, 0x6474e551)]
    headers += [(2, 4, dynamic_offset, 0, 0, len(dynamics), len(dynamics), 8),
                (0x61000000, 4, data_offset, 0, 0, len(data) + len(dynamics), 0, 8)]
    phoff = len(image); image.extend(b"".join(struct.pack("<IIQQQQQQ", *header) for header in headers))
    image[7:9] = bytes((9, 2)); struct.pack_into("<H", image, 16, 0xfe10 if main else 0xfe18)
    struct.pack_into("<QQ", image, 32, phoff, 0); struct.pack_into("<HHHH", image, 56, len(headers), 0, 0, 0)
    return image, addresses, {"symbols": {name: data_offset + symbol_offset + offset for name, offset in locations.items()},
                              "tags": {key: dynamic_offset + index * 16 + 8 for key, index in tag_locations.items()},
                              "names": {key: data_offset + string_offset + offset for key, offset in name_locations.items()},
                              "string_base": data_offset + string_offset, "phoff": phoff, "headers": headers}


def build(provider_linked, consumer_linked, main_linked, destination):
    output = Path(destination); output.mkdir(parents=True, exist_ok=True)
    provider, provider_addresses, pm = package(provider_linked, "provider")
    consumer, consumer_addresses, cm = package(consumer_linked, "consumer")
    main, main_addresses, mm = package(main_linked, "main")
    def emit(case, m, p, c=None):
        directory = output / case; directory.mkdir(exist_ok=True)
        (directory / "SceInternalMain.elf").write_bytes(m)
        (directory / "SceInternalLibc.prx").write_bytes(p)
        (directory / "SceInternalConsumer.prx").write_bytes(consumer if c is None else c)
        return f"{case} {hashlib.sha256(p).hexdigest()} {len(p)}"
    records = [emit("valid", main, provider)]
    for case, kind, target, value in (
            ("consumer-library", "name", (1, "library"), None),
            ("consumer-module", "name", (1, "module"), None),
            ("consumer-library-version", "tag", (1, "library"), 2 << 32),
            ("consumer-module-major", "tag", (1, "module"), 0x201 << 32),
            ("consumer-module-minor", "tag", (1, "module"), 0x102 << 32)):
        changed = bytearray(main)
        if kind == "name": changed[mm["names"][target]] = ord('X')
        else:
            offset = mm["tags"][target]; previous = struct.unpack_from("<Q", changed, offset)[0]
            struct.pack_into("<Q", changed, offset, (previous & ~(0xffff << 32)) | value)
        records.append(emit(case, changed, provider))
    for case, target, field, value in (("consumer-type", "memcpy", 4, 0x11),):
        changed = bytearray(main); changed[mm["symbols"][target] + field] = value
        records.append(emit(case, changed, provider))
    changed = bytearray(main); name = struct.unpack_from("<I", changed, mm["symbols"]["memcpy"])[0]
    changed[mm["string_base"] + name] = ord('X'); records.append(emit("consumer-nid", changed, provider))
    unsupported_main, unsupported_provider, unsupported_consumer = bytearray(main), bytearray(provider), bytearray(consumer)
    for changed, metadata in ((unsupported_main, mm), (unsupported_provider, pm), (unsupported_consumer, cm)):
        name = struct.unpack_from("<I", changed, metadata["symbols"]["memcpy"])[0]
        offset = metadata["string_base"] + name
        changed[offset:offset + 11] = nid("sceLibcMspaceCalloc").encode("ascii")
    records.append(emit("matched-unsupported-nid", unsupported_main, unsupported_provider, unsupported_consumer))
    for case, mutation in (("provider-type", "type"), ("provider-ambiguity", "duplicate"),
                           ("provider-storage", "storage"), ("provider-permission", "permission"),
                           ("provider-relro", "relro"), ("provider-module", "module"),
                           ("provider-library", "library"), ("provider-library-version", "version")):
        changed = bytearray(provider); symbol = pm["symbols"]["memcpy"]
        if mutation == "type": changed[symbol + 4] = 0x11
        elif mutation == "duplicate":
            struct.pack_into("<I", changed, pm["symbols"]["SceInternalTwin"], struct.unpack_from("<I", changed, symbol)[0])
        elif mutation == "storage":
            code, = [header for header in pm["headers"] if header[0] == 1 and header[1] & 1]
            code_index = pm["headers"].index(code)
            if any(h[0] == 1 and code[3] < h[3] <= code[3] + code[5] for h in pm["headers"]):
                raise ValueError("No room for independent executable BSS storage control")
            struct.pack_into("<Q", changed, pm["phoff"] + code_index * 56 + 40, code[5] + 1)
            struct.pack_into("<QQ", changed, symbol + 8, code[3] + code[5], 1)
        elif mutation in ("library", "version"):
            name = struct.unpack_from("<I", changed, symbol)[0]
            changed[pm["string_base"] + name + 12] = ord('E' if mutation == "library" else 'F')
        elif mutation == "module": changed[pm["names"][(0, "module")]] = ord('X')
        elif mutation == "relro":
            code, = [header for header in pm["headers"] if header[0] == 1 and header[1] & 1]
            code_index = pm["headers"].index(code)
            # Extend the existing executable LOAD across its linker padding so
            # the independent RELRO control is a valid, fully mapped page.
            struct.pack_into("<QQ", changed, pm["phoff"] + code_index * 56 + 32, 4096, 4096)
            changed.extend(struct.pack("<IIQQQQQQ", 0x6474e552, 4, code[2], code[3] & ~4095, 0, 4096, 4096, 1))
            struct.pack_into("<H", changed, 56, len(pm["headers"]) + 1)
        else:
            # Preserve executable initializers and all other function exports;
            # only memcpy's typed target points into readable/writable BSS.
            struct.pack_into("<QQ", changed, symbol + 8, provider_addresses["SceInternalState"], 1)
        records.append(emit(case, main, changed))
    for case, owner, target, field, value in (
            ("consumer-size", "main", "sceLibcMspaceMalloc", 16, 1 << 32),
            ("provider-reserved", "provider", "sceLibcMspaceMalloc", 6, 0xfff1),
            ("provider-visibility", "provider", "sceLibcMspaceMalloc", 5, 2),
            ("provider-binding", "provider", "sceLibcMspaceMalloc", 4, 0x02)):
        changed = bytearray(main if owner == "main" else provider)
        metadata = mm if owner == "main" else pm
        offset = metadata["symbols"][target] + field
        struct.pack_into("<" + ("Q" if field == 16 else "H" if field == 6 else "B"), changed, offset, value)
        records.append(emit(case, changed if owner == "main" else main, changed if owner == "provider" else provider))
    first = [provider_addresses["SceInternalState"], provider_addresses["SceInternalPrinted"],
             main_addresses["SceInternalAddresses"], main_addresses["SceInternalOutput"],
             *[provider_addresses[name] for name in FUNCTIONS[:9]], consumer_addresses["SceInternalConsumerState"],
             *[provider_addresses[name] for name in FUNCTIONS[9:]],
             *[main_addresses[name] for name in ("SceInternalExtraOutput", "SceInternalArenas", "SceInternalStats", "SceInternalTrace")],
             provider_addresses["SceInternalExtra"],
             *[main_addresses[name] for name in ("SceInternalAbortProbe", "SceInternalPureProbe", "SceInternalOwnershipProbe")]]
    (output / "receipt.txt").write_text(" ".join(map(str, first)) + "\n" + "\n".join(records) + "\n")
    (output / "receipt.json").write_text(json.dumps({"provider_symbol_values": provider_addresses,
        "main_symbol_values": main_addresses, "sources": records,
        "independent_formatted_output": "guest:11,22,33,44,55,66,77:3.5",
        "consumer_sha256": hashlib.sha256(consumer).hexdigest(),
        "expected_provider_counters": [1, 1, 2, 1, 1, 2, 3, 2, 3, 1, 4]}, indent=2) + "\n")


if __name__ == "__main__": build(*sys.argv[1:])
