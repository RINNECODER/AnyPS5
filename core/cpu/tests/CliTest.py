import json
import math
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zlib


runner, fixture = sys.argv[1:]


def run(*arguments):
    return subprocess.run([runner, *map(str, arguments)], capture_output=True,
                          text=True, timeout=20)


def events(result):
    values = [json.loads(line) for line in result.stderr.splitlines()]
    assert all(value["schema_version"] == 1 for value in values), values
    return values


capabilities = run("--capabilities-json")
assert capabilities.returncode == 0 and capabilities.stderr == "", capabilities
support = json.loads(capabilities.stdout)
assert support["schema_version"] == 1, support
assert support["host_architecture"] == "arm64", support
assert support["guest_architecture"] == "x86_64", support
assert support["backend"] == "Unicorn 2 x86-64 dynamic translation", support
assert support["supported_formats"] == ["static_elf64_x86_64"], support
assert support["runtime_abi"] == "linux_sysv", support
assert support["ps5_game_runtime_ready"] is False, support
assert {(value["name"], value["number"]) for value in support["services"]} == {
    ("write", 1), ("exit", 60), ("exit_group", 231), ("arch_prctl", 158)}, support

limit = 31
primes = [number for number in range(2, limit + 1)
          if all(number % divisor for divisor in range(2, math.isqrt(number) + 1))]
pixels = bytes(((index * 37 + (index >> 3)) ^ (index >> 5)) & 255
               for index in range(4096))
expected = (f"homebrew primes={len(primes)} sum={sum(primes)} "
            f"buffer_crc32={zlib.crc32(pixels)} tls=ok bss=ok\n")
success = run("--diagnostics-json", fixture, limit)
assert success.returncode == 0 and success.stdout == expected, success
started, exited = events(success)
assert started["event"] == "startup" and started["executable"] == fixture, started
assert started["entry"] > 0 and started["host_architecture"] == "arm64", started
assert started["guest_architecture"] == "x86_64" and started["backend"] == support["backend"], started
assert exited == {"schema_version": 1, "event": "guest_exit", "exit_code": 0}, exited

rejected = run("--diagnostics-json", fixture, "--not-a-host-option")
assert rejected.returncode == 2 and rejected.stdout == "", rejected
assert [value["event"] for value in events(rejected)] == ["startup", "guest_exit"], rejected
assert events(rejected)[-1]["exit_code"] == 2, rejected

with tempfile.TemporaryDirectory() as directory:
    unusual = pathlib.Path(directory) / 'guest "quoted" \\ path\n café.elf'
    shutil.copyfile(fixture, unusual)
    escaped = run("--diagnostics-json", unusual, limit)
    assert escaped.returncode == 0 and escaped.stdout == expected, escaped
    assert events(escaped)[0]["executable"] == str(unusual), escaped

    missing = pathlib.Path(directory) / 'missing "quoted" \\ path\n café.elf'
    unavailable = run("--diagnostics-json", missing)
    assert unavailable.returncode == 126 and unavailable.stdout == "", unavailable
    error, = events(unavailable)
    assert error["event"] == "error" and error["code"] == "input_unavailable", error
    assert error["executable"] == str(missing) and str(missing) in error["message"], error
    assert error["process_exit"] == 126, error

    invalid = pathlib.Path(directory) / "invalid.elf"
    invalid.write_bytes(b"not an ELF executable".ljust(128, b"\0"))
    unsupported = run("--diagnostics-json", invalid)
    assert unsupported.returncode == 126 and unsupported.stdout == "", unsupported
    error, = events(unsupported)
    assert error["event"] == "error" and error["code"] == "unsupported_executable", error
    assert error["process_exit"] == 126 and error["executable"] == str(invalid), error

    default = run(invalid)
    assert default.returncode == 126 and default.stdout == "", default
    assert default.stderr == "anyps5_cpu_run: ELF loader: input is not an ELF executable\n", default

for arguments in (("--unknown",), ("--capabilities-json", fixture), (),
                  ("--diagnostics-json",), ("--diagnostics-json", "--unknown")):
    invalid = run(*arguments)
    assert invalid.returncode == 126 and invalid.stdout == "", invalid
    if arguments and arguments[0] == "--diagnostics-json":
        error, = events(invalid)
        assert error["event"] == "error" and error["code"] == "invalid_arguments", error
    else:
        assert invalid.stderr.startswith("anyps5_cpu_run: "), invalid

print("PASS: CLI capability truth, typed startup/exit/error JSONL, actual homebrew output, guest exit propagation, argument boundary and escaped paths")
