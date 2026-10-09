"""Game runs are unbounded by default; instruction, wall and idle caps are opt-in CLI flags.

The guest is a minimal static ELF that retires a known number of instructions and
then exits with its own status, so even the unbounded run is finite.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


runner, = sys.argv[1:]
OLD_ENTRY_BUDGET = 100_000_000
ITERATIONS = 55_000_000
RETIRED = 1 + 2 * ITERATIONS + 3  # mov; ITERATIONS x (dec, jnz); mov, mov, syscall
STATUS = 7
assert RETIRED > OLD_ENTRY_BUDGET


def spinning_elf(iterations):
    base = 0x400000
    code = (b"\xb9" + struct.pack("<I", iterations)    # mov ecx, iterations
            + b"\x48\xff\xc9" + b"\x75\xfb"             # 1: dec rcx; jnz 1b
            + b"\xbf" + struct.pack("<I", STATUS)       # mov edi, STATUS
            + b"\xb8\x3c\x00\x00\x00" + b"\x0f\x05")    # mov eax, 60 (exit); syscall
    entry = 64 + 56
    size = entry + len(code)
    header = (b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
              + struct.pack("<HHIQQQIHHHHHH", 2, 62, 1, base + entry, 64, 0, 0, 64, 56, 1, 0, 0, 0))
    segment = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base, size, size, 0x1000)
    return header + segment + code


def run(*arguments, timeout=20):
    return subprocess.run([runner, *map(str, arguments)], capture_output=True, text=True, timeout=timeout)


def events(result):
    values = [json.loads(line) for line in result.stderr.splitlines()]
    assert all(value["schema_version"] == 1 for value in values), result
    return values


def capabilities(*limits):
    result = run(*limits, "--capabilities-json")
    assert result.returncode == 0 and result.stderr == "", result
    return json.loads(result.stdout)


support = capabilities()
native = "native_module_runner" in support
assert support["execution_limits"] == {"max_instructions": 0, "max_init_instructions": 0}, support
if native:
    contract = support["native_module_runner"]
    assert contract["wall_limit_ms"] == 0 and contract["idle_limit_ms"] == 0, contract
limits = ["--max-instructions", 5, "--max-init-instructions", 6]
if native:
    limits += ["--max-wall-ms", 1000, "--max-idle-ms", 50]
reported = capabilities(*limits)
assert reported["execution_limits"] == {"max_instructions": 5, "max_init_instructions": 6}, reported
if native:
    contract = reported["native_module_runner"]
    assert contract["wall_limit_ms"] == 1000 and contract["idle_limit_ms"] == 50, contract
print("capabilities report unbounded defaults (0) and the effective explicit limits PASS")

with tempfile.TemporaryDirectory(prefix="anyps5-run-limits-") as directory:
    guest = Path(directory) / "spin.elf"
    guest.write_bytes(spinning_elf(ITERATIONS))

    unbounded = run("--diagnostics-json", guest, timeout=120)
    assert unbounded.returncode == STATUS and unbounded.stdout == "", unbounded
    assert [value["event"] for value in events(unbounded)] == ["startup", "guest_exit"], unbounded
    assert events(unbounded)[-1]["exit_code"] == STATUS, unbounded
    print(f"default run retires {RETIRED} instructions past the old {OLD_ENTRY_BUDGET} cap and exits {STATUS} PASS")

    for budget, status in ((150_000_000, STATUS), (1_000_000, None)):
        bounded = run("--diagnostics-json", "--max-instructions", budget, guest, timeout=120)
        if status is None:
            assert bounded.returncode == 126 and bounded.stdout == "", bounded
            error = events(bounded)[-1]
            assert error["event"] == "error" and error["code"] == "execution_limit", bounded
        else:
            assert bounded.returncode == status and events(bounded)[-1]["exit_code"] == status, bounded
    print("--max-instructions above the old cap admits the guest; a smaller explicit budget stops it PASS")

    invalid = [["--max-instructions"], ["--max-instructions", "-1"], ["--max-instructions", "12x"],
               ["--max-instructions", "18446744073709551616"],
               ["--max-instructions", 1, "--max-instructions", 1],
               ["--max-init-instructions", "abc"]]
    if native:
        invalid += [["--max-wall-ms", "-5"], ["--max-idle-ms", "1.5"],
                    ["--max-wall-ms", 1, "--max-wall-ms", 1],
                    ["--max-wall-ms", 1000]]  # native limits need a native SCE module graph
    else:
        invalid += [["--max-wall-ms", 1000], ["--max-idle-ms", 1000]]
    for arguments in invalid:
        rejected = run("--diagnostics-json", *arguments, guest)
        assert rejected.returncode == 126 and rejected.stdout == "", (arguments, rejected)
        error, = events(rejected)
        assert error["event"] == "error" and error["code"] == "invalid_arguments", (arguments, rejected)
    print("malformed, duplicate and unsupported limit flags are rejected before guest entry PASS")
