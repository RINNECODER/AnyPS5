import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


runner, fixture = sys.argv[1:]


def inspect(path):
    result = subprocess.run([runner, "--inspect-sce-json", str(path)], capture_output=True,
                            text=True, timeout=20)
    assert result.returncode == 0, (result.returncode, result.stderr)
    assert result.stderr == "", result.stderr
    return json.loads(result.stdout)


metadata = inspect(fixture)
assert metadata["type"] == 0xfe10 and metadata["os_abi"] == 9 and metadata["abi_version"] == 2, metadata
assert metadata["format"] == "sce_elf64_x86_64", metadata
assert metadata["unsupported_reasons"] == [] and metadata["needed_files"] == [], metadata
assert metadata["needed_modules"] == ["libc", "libkernel", "libSceUserService", "libSceSystemService"] and not metadata["has_tls"], metadata
assert metadata["relocation_count"] >= 31 and set(metadata["relocation_types"]) == {7, 8}, metadata
libc_nids = {"Q3VBxCXhUHs", "+P6FRGH4LfA", "8zTFvBIAIN8", "j4ViWNHEgww", "Ovb2dSJOAuE", "uMei1W9uyNo"}
kernel_nids = {"1G3lF1Gg1k8", "Cg4srZ6TKbU", "+r3rMFwItV4", "oib76F-12fk", "UK2Tl2DWUns",
               "pO96TwzOm5E", "C0f7TJcbfac", "rTXw65xmLIA", "B+vc2AO2Zrc", "L-Q3LEjIbgA",
               "IWIBBdTHit4", "7oxv3PPCumo", "vSMAm3cxYTY", "rVjRvHJ0X6c", "cQke9UuBQOk", "MBuItvba6z8"}
user_nids = {"j3YMu1MVNNo", "CdWp0oHWGr0", "fPhymKNvK-A", "1xxcMiGu2fo"}
system_nids = {"fZo48un7LK4", "SsC-m-S9JTA", "Vo5V8KAwCmk"}
expected_nids = libc_nids | kernel_nids | user_nids | system_nids
assert len(metadata["imports"]) == 29 and {value["nid"] for value in metadata["imports"]} == expected_nids, metadata
for value in metadata["imports"]:
    library, local_id = (("libc", 1) if value["nid"] in libc_nids else
                         ("libkernel", 2) if value["nid"] in kernel_nids else
                         ("libSceUserService", 3) if value["nid"] in user_nids else ("libSceSystemService", 4))
    assert value["library"] == library and value["module"] == library, value
    assert value["library_id"] == value["module_id"] == local_id and value["library_version"] == 1, value
    assert value["module_major"] == value["module_minor"] == 1, value
print("compiled SCE inspection: twenty-nine qualified libc/libkernel/UserService/SystemService NIDs and real PLT/RELATIVE tables PASS")


def oracle(limit, seed):
    primes = [number for number in range(2, limit + 1)
              if all(number % divisor for divisor in range(2, math.isqrt(number) + 1))]
    pixels = bytes(((index * 37 + (index >> 3) + seed * 17) ^ (index >> 5)) & 255
                   for index in range(4096))
    return len(primes), sum(primes), zlib.adler32(pixels)


resource_bytes = bytes(((index * 41) ^ (index >> 1) ^ 0xa3) & 255 for index in range(37))


def memory_oracle(seed):
    return zlib.adler32(bytes(((index * 29 + (index >> 2) + seed * 11) ^ (index >> 4)) & 255
                             for index in range(4096)))


def execute(arguments, expected, content=resource_bytes, cwd_default=False,
            resource_checksum=None, create_resource=True, memory_checksum=None):
    checksum = zlib.adler32(content) if resource_checksum is None else resource_checksum
    memory_checksum = memory_oracle(int(arguments[1])) if memory_checksum is None else memory_checksum
    with tempfile.TemporaryDirectory(prefix="anyps5 resource é ") as directory:
        root = Path(directory) / "root with spaces"
        root.mkdir()
        if create_resource:
            (root / "resource.bin").write_bytes(content)
        root_options = [] if cwd_default else ["--resource-root", str(root)]
        result = subprocess.run([runner, *root_options, "--diagnostics-json", fixture,
                                 *map(str, arguments), str(len(content)), str(checksum), str(memory_checksum)],
                                cwd=root if cwd_default else directory,
                                capture_output=True, text=True, timeout=20)
    assert result.returncode == expected, (arguments, expected, result.returncode, result.stderr)
    assert result.stdout == "", result.stdout
    events = [json.loads(line) for line in result.stderr.splitlines()]
    assert [event["event"] for event in events] == ["startup", "guest_exit"], events
    assert events[0]["host_architecture"] == "arm64", events
    assert events[0]["guest_architecture"] == "x86_64", events
    assert events[0]["format"] == "sce_elf64_x86_64", events
    assert events[1]["exit_code"] == expected, events


for limit, seed in ((2, 0), (31, 1), (1000, 91), (4095, 255)):
    expected = oracle(limit, seed)
    execute((limit, seed, *expected, "import"), 0)
    print(f"SCE x86 homebrew limit={limit} seed={seed}: primes={expected[0]} "
          f"sum={expected[1]} adler32={expected[2]} memory_adler32={memory_oracle(seed)}, imported exit PASS")

expected = oracle(1000, 91)
for index in range(3):
    wrong = list(expected)
    wrong[index] ^= 1
    execute((1000, 91, *wrong, "import"), 77)
print("independently wrong prime count, prime sum, and full Adler32: rejected with guest exit77")
execute((1000, 91, *expected, "import"), 77, memory_checksum=memory_oracle(91) ^ 1)
print("compiled guest memory imports, virtual 12 GiB capacity, aliases, RX code returning1234, query, flexible and reserved ranges: PASS")
print("independently wrong shared-alias Adler32: rejected with guest exit77")
print("compiled guest UserService NULL initialization, ID 0x10000000, four login slots, Player name and output guards: PASS")
print("compiled guest SystemService English/UTC/summertime integers, AnyPS5 name with untouched capacity tail, output guards and repeated hide splash: PASS")

execute((1000, 91, *expected, "import"), 0, cwd_default=True)
execute((1000, 91, *expected, "import"), 77, resource_checksum=zlib.adler32(resource_bytes) ^ 1)
execute((1000, 91, *expected, "import"), 94, create_resource=False)
for length in (10, 127):
    content = bytes((index * 13 + length) & 255 for index in range(length))
    execute((31, 1, *oracle(31, 1), "import"), 0, content=content)
print("compiled guest resource bytes, read/pread/seek/EOF/errors, explicit root and cwd default: PASS")
print("independently wrong resource Adler and missing real resource: guest exits77/94 PASS")

with tempfile.TemporaryDirectory(prefix="anyps5-sce-callback-") as directory:
    (Path(directory) / "resource.bin").write_bytes(resource_bytes)
    callback = subprocess.run([runner, "--diagnostics-json", "--resource-root", directory, fixture,
                               "1000", "91", *map(str, expected), "callback",
                               str(len(resource_bytes)), str(zlib.adler32(resource_bytes)), str(memory_oracle(91))],
                              capture_output=True, text=True, timeout=20)
assert callback.returncode == 126 and callback.stdout == "", (callback.returncode, callback.stdout, callback.stderr)
events = [json.loads(line) for line in callback.stderr.splitlines()]
assert [event["event"] for event in events] == ["startup", "error"], events
assert events[1]["code"] == "unsupported_service" and "Unsupported SCE entry termination callback" in events[1]["message"], events
print("compiled no-argument SCE entry callback: explicit unsupported service PASS")

with tempfile.TemporaryDirectory(prefix="anyps5-sce-unsupported-open-") as directory:
    resource_path = Path(directory) / "resource.bin"
    resource_path.write_bytes(resource_bytes)
    unsupported = subprocess.run([runner, "--diagnostics-json", "--resource-root", directory, fixture,
                                  "1000", "91", *map(str, expected), "unsupported-flags",
                                  str(len(resource_bytes)), str(zlib.adler32(resource_bytes)), str(memory_oracle(91))],
                                 capture_output=True, text=True, timeout=20)
    assert resource_path.read_bytes() == resource_bytes, "Unsupported guest open changed actual resource bytes"
assert unsupported.returncode == 126 and unsupported.stdout == "", (unsupported.returncode, unsupported.stdout, unsupported.stderr)
events = [json.loads(line) for line in unsupported.stderr.splitlines()]
assert [event["event"] for event in events] == ["startup", "error"], events
assert events[1]["code"] == "unsupported_service" and "Unsupported guest /app0 open flags" in events[1]["message"], events
print("compiler-produced unsupported write-open: accurate CLI service classification and unchanged resource PASS")

for arguments in ((1, 91, *expected, "callback"), (4096, 91, *expected, "callback"),
                  (1000, 256, *expected, "callback"), ("abc", 91, *expected, "callback"),
                  (1000, 91, "4294967296", expected[1], expected[2], "callback"),
                  (1000, 91, "", expected[1], expected[2], "callback")):
    execute(arguments, 82)
execute((1000, 91, *expected, "unknown"), 83)
execute((1000, 91), 81)
print("SCE argv validation and exit propagation: PASS")

original = Path(fixture).read_bytes()
phoff, = struct.unpack_from("<Q", original, 32)
phsize, phcount = struct.unpack_from("<HH", original, 54)
headers = [struct.unpack_from("<IIQQQQQQ", original, phoff + index * phsize) for index in range(phcount)]
dynamic = next(value for value in headers if value[0] == 2)
with tempfile.TemporaryDirectory(prefix="anyps5-sce-homebrew-") as directory:
    mutations = []
    for name, tag, local_id, version, diagnostic, scope in (
            ("user", 0x61000049, 3, 2, "Unsupported SCE user import scope/version", "library=libSceUserService:2"),
            ("system", 0x61000045, 4, 0x0102, "Unsupported SCE system import scope/version", "module=libSceSystemService:1.2")):
        wrong_version = bytearray(original)
        identity_tag = next(offset for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16)
                            if struct.unpack_from("<Q", original, offset)[0] == tag and
                            struct.unpack_from("<Q", original, offset + 8)[0] >> 48 == local_id)
        value, = struct.unpack_from("<Q", wrong_version, identity_tag + 8)
        struct.pack_into("<Q", wrong_version, identity_tag + 8, (value & ~(0xffff << 32)) | (version << 32))
        path = Path(directory) / f"unsupported-{name}-version.elf"
        path.write_bytes(wrong_version)
        result = subprocess.run([runner, "--diagnostics-json", str(path)], capture_output=True,
                                text=True, timeout=20)
        assert result.returncode == 126 and result.stdout == "", (result.returncode, result.stdout, result.stderr)
        events = [json.loads(line) for line in result.stderr.splitlines()]
        assert len(events) == 1 and events[0]["event"] == "error" and events[0]["code"] == "unsupported_executable", events
        assert diagnostic in events[0]["message"] and scope in events[0]["message"], events
    tls = bytearray(original)
    table = bytearray(original[phoff:phoff + phcount * phsize])
    data = next(value for value in headers if value[0] == 1 and value[1] & 2)
    table += struct.pack("<IIQQQQQQ", 7, 4, data[2] + 1, data[3] + 1, data[4] + 1, 1, 16, 16)
    new_table = len(tls)
    tls.extend(table)
    struct.pack_into("<Q", tls, 32, new_table)
    struct.pack_into("<H", tls, 56, phcount + 1)
    mutations.append(("tls", tls, "unsupported TLS alignment residue", False))
    initializer = bytearray(original)
    new_dynamic = bytearray(original[dynamic[2]:dynamic[2] + dynamic[5] - 16])
    new_dynamic += struct.pack("<QQQQ", 12, metadata["entry"], 0, 0)
    new_offset = len(initializer)
    initializer.extend(new_dynamic)
    index = next(index for index, value in enumerate(headers) if value[0] == 2)
    struct.pack_into("<IIQQQQQQ", initializer, phoff + index * phsize,
                     2, 4, new_offset, 0, 0, len(new_dynamic), len(new_dynamic), 8)
    mutations.append(("initializer", initializer, "initializer or finalizer execution is unsupported", True))
    for name, image, expected_reason, inspectable in mutations:
        path = Path(directory) / f"{name}.elf"
        path.write_bytes(image)
        if inspectable:
            blocked = inspect(path)
            assert any(expected_reason in reason for reason in blocked["unsupported_reasons"]), blocked
        else:
            invalid = subprocess.run([runner, "--diagnostics-json", "--inspect-sce-json", str(path)],
                                     capture_output=True, text=True, timeout=20)
            assert invalid.returncode == 126 and invalid.stdout == "", (invalid.returncode, invalid.stdout, invalid.stderr)
            events = [json.loads(line) for line in invalid.stderr.splitlines()]
            assert len(events) == 1 and events[0]["event"] == "error" and expected_reason in events[0]["message"], events
        result = subprocess.run([runner, "--diagnostics-json", str(path)], capture_output=True,
                                text=True, timeout=20)
        assert result.returncode == 126 and result.stdout == "", (name, result.returncode, result.stdout, result.stderr)
        events = [json.loads(line) for line in result.stderr.splitlines()]
        assert len(events) == 1 and events[0]["event"] == "error", events
        assert expected_reason in events[0]["message"], events
print("compiled SCE malformed TLS alignment and initializer requirements: rejected before guest entry PASS")
print("compiled UserService library and SystemService module version mismatches: qualified CLI routes reject before guest entry PASS")
