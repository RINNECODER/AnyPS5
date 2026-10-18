"""Check that PT_GNU_STACK survives the Linux executable conversion and a missing one is synthesised."""

from pathlib import Path
import struct
import subprocess
import sys
import tempfile

PT_NULL = 0
PT_LOAD = 1
PT_DYNAMIC = 2
PT_GNU_EH_FRAME = 0x6474E550
PT_GNU_STACK = 0x6474E551
PT_GNU_RELRO = 0x6474E552
PT_SCE_VERSION = 0x6FFFFF01
BASE = 0x400000


def fixture(with_stack=True, extra_slots=3):
    image = bytearray(0x8000)
    image[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    header_count = 2 + (1 if with_stack else 0) + extra_slots
    struct.pack_into("<HHIQQQIHHHHHH", image, 16,
                     3, 62, 1, 0x4000, 64, 0, 0, 64, 56, header_count, 64, 0, 0)
    image[0x4000:0x4006] = b"\xb8\x2a\x00\x00\x00\xc3"
    tags = [(5, 0x600), (10, 1), (6, 0x620), (11, 24), (7, 0x700), (8, 0), (9, 24), (0, 0)]
    struct.pack_into("<IIQQQQQQ", image, 64,
                     PT_LOAD, 5, 0x4000, 0, 0, 0x1000, 0x1000, 0x4000)
    struct.pack_into("<IIQQQQQQ", image, 120,
                     PT_DYNAMIC, 6, 0x600 + 0x4000, 0x600, 0x600, len(tags) * 16, len(tags) * 16, 8)
    next_header = 2
    if with_stack:
        struct.pack_into("<IIQQQQQQ", image, 64 + next_header * 56,
                         PT_GNU_STACK, 6, 0, 0, 0, 0, 0x20000, 16)
        next_header += 1
    for index in range(next_header, header_count):
        struct.pack_into("<IIQQQQQQ", image, 64 + index * 56, PT_SCE_VERSION, 0, 0, 0, 0, 0, 0, 1)
    for index, tag in enumerate(tags):
        struct.pack_into("<qQ", image, 0x4600 + index * 16, *tag)
    return image


def headers(elf):
    phoff, = struct.unpack_from("<Q", elf, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x36)
    return [struct.unpack_from("<IIQQQQQQ", elf, phoff + index * phentsize) for index in range(phnum)]


def relink(relinker, directory, name, image, *options):
    source = Path(directory) / (name + ".elf")
    output = Path(directory) / (name + ".out")
    source.write_bytes(image)
    result = subprocess.run([str(relinker), "--skip-sce-module", *options, str(source), str(output)],
                            capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, (name, result.returncode, result.stdout, result.stderr)
    return result, output.read_bytes()


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-gnu-stack-") as directory:
        _, elf = relink(relinker, directory, "kept", fixture())
        output_headers = headers(elf)
        assert (PT_GNU_STACK, 6, 0, 0, 0, 0, 0x20000, 16) in output_headers, output_headers
        assert all(header[0] != PT_SCE_VERSION for header in output_headers), output_headers

        _, elf = relink(relinker, directory, "kept-fixed", fixture(), "--linux-fixed-base", hex(BASE))
        fixed_headers = headers(elf)
        assert (PT_GNU_STACK, 6, 0, 0, 0, 0, 0x20000, 16) in fixed_headers, fixed_headers

        _, elf = relink(relinker, directory, "no-stack", fixture(with_stack=False))
        output_headers = headers(elf)
        assert [header for header in output_headers if header[0] == PT_GNU_STACK] == [(PT_GNU_STACK, 6, 0, 0, 0, 0, 0, 16)], output_headers

        _, elf = relink(relinker, directory, "no-stack-fixed", fixture(with_stack=False), "--linux-fixed-base", hex(BASE))
        fixed_headers = headers(elf)
        assert [header for header in fixed_headers if header[0] == PT_GNU_STACK] == [(PT_GNU_STACK, 6, 0, 0, 0, 0, 0, 16)], fixed_headers
        assert all(header[0] != PT_SCE_VERSION for header in fixed_headers), fixed_headers
    print("Linux GNU stack tests passed")


if __name__ == "__main__":
    main()
