"""Check the opt-in indirect-branch tracer (ANYPS5_TRACE_INDIRECT) rewrites only computed branches."""

from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile

from test_linux_load_alignment import fixture


# Guest virtual address 0x10: call rax; jmp rax; call [rax+8]; ret.
SITES = bytes.fromhex("ff d0 ff e0 ff 50 08 c3")
TRAPPED = bytes.fromhex("cc d0 cc e0 cc 50 08")
SITE_ADDRESSES = (0x10, 0x12, 0x14)


def executable():
    image = fixture()
    struct.pack_into("<Q", image, 24, 0x4010)
    image[0x4010:0x4010 + len(SITES)] = SITES
    return image


def relink(relinker, directory, name, env=None):
    source = Path(directory) / f"{name}.elf"
    output = Path(directory) / f"{name}.out"
    source.write_bytes(bytes(executable()))
    result = subprocess.run([str(relinker), "--skip-sce-module", str(source), str(output)],
                            capture_output=True, text=True, timeout=30, env=env)
    return result, output


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-indirect-") as directory:
        work = Path(directory)
        environment = dict(os.environ)
        environment["ANYPS5_TRACE_INDIRECT"] = str(work / "trace.bin")

        result, output = relink(relinker, work, "traced", env=environment)
        assert result.returncode == 0, (result.stdout, result.stderr)
        data = output.read_bytes()
        assert TRAPPED in data, "computed indirect branches were not rewritten to int3"
        assert SITES not in data, "the original indirect branches survived the traced relink"

        sites = (work / "trace.bin.sites").read_bytes()
        magic, count, begin, end = struct.unpack_from("<IIQQ", sites, 0)
        assert magic == 0x49355041, hex(magic)
        assert (count, begin, end) == (len(SITE_ADDRESSES), 0, 0x1000), (count, begin, end)
        assert struct.unpack_from(f"<{count}Q", sites, 24) == SITE_ADDRESSES

        result, output = relink(relinker, work, "plain")
        assert result.returncode == 0, (result.stdout, result.stderr)
        data = output.read_bytes()
        assert SITES in data and TRAPPED not in data, "a relink without ANYPS5_TRACE_INDIRECT changed the code"
    print("guest indirect trap test passed")


if __name__ == "__main__":
    main()
