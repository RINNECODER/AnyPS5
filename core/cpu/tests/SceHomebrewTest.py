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
assert metadata["needed_modules"] == ["libc"] and not metadata["has_tls"], metadata
assert metadata["relocation_count"] >= 8 and set(metadata["relocation_types"]) == {7, 8}, metadata
expected_nids = {"Q3VBxCXhUHs", "+P6FRGH4LfA", "8zTFvBIAIN8", "j4ViWNHEgww", "Ovb2dSJOAuE", "uMei1W9uyNo"}
assert {value["nid"] for value in metadata["imports"]} == expected_nids, metadata
for value in metadata["imports"]:
    assert value["library"] == "libc" and value["module"] == "libc", value
    assert value["library_id"] == value["module_id"] == value["library_version"] == 1, value
    assert value["module_major"] == value["module_minor"] == 1, value
print("compiled SCE inspection: six independently known qualified NIDs and real PLT/RELATIVE tables PASS")


def oracle(limit, seed):
    primes = [number for number in range(2, limit + 1)
              if all(number % divisor for divisor in range(2, math.isqrt(number) + 1))]
    pixels = bytes(((index * 37 + (index >> 3) + seed * 17) ^ (index >> 5)) & 255
                   for index in range(4096))
    return len(primes), sum(primes), zlib.adler32(pixels)


def execute(arguments, expected):
    result = subprocess.run([runner, "--diagnostics-json", fixture, *map(str, arguments)],
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
          f"sum={expected[1]} adler32={expected[2]}, imported exit PASS")

expected = oracle(1000, 91)
for index in range(3):
    wrong = list(expected)
    wrong[index] ^= 1
    execute((1000, 91, *wrong, "import"), 77)
print("independently wrong prime count, prime sum, and full Adler32: rejected with guest exit77")

callback = subprocess.run([runner, "--diagnostics-json", fixture, "1000", "91", *map(str, expected), "callback"],
                          capture_output=True, text=True, timeout=20)
assert callback.returncode == 126 and callback.stdout == "", (callback.returncode, callback.stdout, callback.stderr)
events = [json.loads(line) for line in callback.stderr.splitlines()]
assert [event["event"] for event in events] == ["startup", "error"], events
assert events[1]["code"] == "unsupported_service" and "Unsupported SCE entry termination callback" in events[1]["message"], events
print("compiled no-argument SCE entry callback: explicit unsupported service PASS")

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
    tls = bytearray(original)
    table = bytearray(original[phoff:phoff + phcount * phsize])
    data = next(value for value in headers if value[0] == 1 and value[1] & 2)
    table += struct.pack("<IIQQQQQQ", 7, 4, data[2], data[3], data[4], 1, 16, 16)
    new_table = len(tls)
    tls.extend(table)
    struct.pack_into("<Q", tls, 32, new_table)
    struct.pack_into("<H", tls, 56, phcount + 1)
    mutations.append(("tls", tls, "TLS initialization is unsupported"))
    initializer = bytearray(original)
    new_dynamic = bytearray(original[dynamic[2]:dynamic[2] + dynamic[5] - 16])
    new_dynamic += struct.pack("<QQQQ", 12, metadata["entry"], 0, 0)
    new_offset = len(initializer)
    initializer.extend(new_dynamic)
    index = next(index for index, value in enumerate(headers) if value[0] == 2)
    struct.pack_into("<IIQQQQQQ", initializer, phoff + index * phsize,
                     2, 4, new_offset, 0, 0, len(new_dynamic), len(new_dynamic), 8)
    mutations.append(("initializer", initializer, "initializer or finalizer execution is unsupported"))
    for name, image, expected_reason in mutations:
        path = Path(directory) / f"{name}.elf"
        path.write_bytes(image)
        blocked = inspect(path)
        assert any(expected_reason in reason for reason in blocked["unsupported_reasons"]), blocked
        result = subprocess.run([runner, "--diagnostics-json", str(path)], capture_output=True,
                                text=True, timeout=20)
        assert result.returncode == 126 and result.stdout == "", (name, result.returncode, result.stdout, result.stderr)
        events = [json.loads(line) for line in result.stderr.splitlines()]
        assert len(events) == 1 and events[0]["event"] == "error", events
        assert expected_reason in events[0]["message"], events
print("compiled SCE TLS and initializer requirements: inspected and rejected before guest entry PASS")
