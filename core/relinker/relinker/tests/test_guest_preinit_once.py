"""A converted guest module's DT_PREINIT_ARRAY runs once, from its own initializer.

The initializer of a converted guest module is the console CRT entry, which walks
DT_PREINIT_ARRAY itself. A relinker startup stub that runs the array too makes every
entry run twice, so a module that registers a thread destructor callback there aborts
on the second registration.
"""
from pathlib import Path
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile

from test_guest_intel_trampolines import main_fixture

ARGS = 0x1122334455667788
ARGP = 0x99AABBCCDDEEFF00
RETURNED = 0x5A17

SHARED = 0x1000
PREINIT0 = 0x1010
PREINIT1 = 0x1030
INIT = 0x1050
RECORD_GOT = 0x2820
PREINIT_SLOTS = 0x2840
IMAGE_BASE = 0x140000000
LOAD_RVA = 0x10000
CALL_SLOT = b'\x48\x8b\x05'
CALL_SUFFIX = b'\x48\x85\xc0\x74\x08\x31\xff\x31\xf6\x31\xd2\xff\xd0'
LC_SEGMENT_64 = 0x19


def elf_header(image, program_headers=4):
    image[:16] = b'\x7fELF\x02\x01\x01' + bytes(9)
    struct.pack_into('<HHIQQQIHHHHHH', image, 16,
                     3, 62, 1, 0, 64, 0, 0, 64, 56, program_headers, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', image, 64, 1, 5, 0, 0, 0, 0x2000, 0x2000, 0x1000)
    struct.pack_into('<IIQQQQQQ', image, 176, 0x6474e551, 6, 0, 0, 0, 0, 0, 16)
    struct.pack_into('<IIQQQQQQ', image, 232, 1, 6, 0x2000, 0x2000, 0x2000, 0x1000, 0x1000, 0x1000)


def recording_module():
    image = bytearray(0x3000)
    elf_header(image)
    strings = b'\0RecordArgs\0libc.prx\0shared\0'
    tags = [(5, 0x2600), (10, len(strings)), (6, 0x2400), (11, 24), (4, 0x2500),
            (7, 0x2700), (8, 72), (9, 24), (1, 12), (12, INIT), (32, PREINIT_SLOTS), (33, 16), (0, 0)]
    struct.pack_into('<IIQQQQQQ', image, 120, 2, 6, 0x2000, 0x2000, 0x2000,
                     len(tags) * 16, len(tags) * 16, 8)
    for index, tag in enumerate(tags):
        struct.pack_into('<qQ', image, 0x2000 + index * 16, *tag)
    image[0x2600:0x2600 + len(strings)] = strings
    struct.pack_into('<IBBHQQ', image, 0x2418, 1, 0x12, 0, 0, 0, 0)
    struct.pack_into('<IBBHQQ', image, 0x2430, 21, 0x12, 0, 1, SHARED, 6)
    struct.pack_into('<IIII', image, 0x2500, 1, 3, 0, 0)
    struct.pack_into('<QQq', image, 0x2700, RECORD_GOT, (1 << 32) | 6, 0)
    struct.pack_into('<QQq', image, 0x2718, PREINIT_SLOTS, 8, PREINIT0)
    struct.pack_into('<QQq', image, 0x2730, PREINIT_SLOTS + 8, 8, PREINIT1)
    image[SHARED:SHARED + 6] = b'\xb8\x2a\x00\x00\x00\xc3'
    for address in (PREINIT0, PREINIT1):
        code = b'\x48\x83\xec\x08\xff\x15' + struct.pack('<i', RECORD_GOT - (address + 10)) + b'\x48\x83\xc4\x08\xc3'
        image[address:address + len(code)] = code
    code = b'\x48\x83\xec\x08'
    for target in (PREINIT0, PREINIT1):
        code += b'\xe8' + struct.pack('<i', target - (INIT + len(code) + 5))
    code += b'\xff\x15' + struct.pack('<i', RECORD_GOT - (INIT + len(code) + 6))
    code += b'\x48\x83\xc4\x08\xb8' + struct.pack('<I', RETURNED) + b'\xc3'
    image[INIT:INIT + len(code)] = code
    return image


def macho_segments(data):
    magic, = struct.unpack_from('<I', data, 0)
    assert magic == 0xFEEDFACF, hex(magic)
    count, = struct.unpack_from('<I', data, 16)
    offset = 32
    segments = []
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, offset)
        if command == LC_SEGMENT_64:
            name = data[offset + 8:offset + 24].rstrip(b'\0').decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, offset + 24)
            segments.append((name, vmaddr, vmsize, fileoff, filesize))
        offset += size
    return segments


def macos_call_targets(data):
    segments = macho_segments(data)

    def address_of(position):
        for _, vmaddr, _, fileoff, filesize in segments:
            if fileoff <= position < fileoff + filesize:
                return vmaddr + position - fileoff
        raise AssertionError(f'Unmapped file offset: {position:#x}')

    targets = []
    position = data.find(CALL_SLOT)
    while position >= 0:
        if data[position + 7:position + 20] == CALL_SUFFIX:
            displacement, = struct.unpack_from('<i', data, position + 3)
            targets.append(address_of(position) + 7 + displacement)
        position = data.find(CALL_SLOT, position + 1)
    return targets


def load_and_record(work, provider, module):
    events = work / 'events.txt'
    run = subprocess.run([sys.executable, __file__, '--load', str(provider), str(module)],
                         env={**os.environ, 'ANYPS5_GUEST_MODULE_ARGS': str(events)},
                         capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, (run.returncode, run.stdout, run.stderr)
    lines = events.read_text().splitlines()
    assert lines == ['args=0000000000000000 argp=0000000000000000',
                     'args=0000000000000000 argp=0000000000000000',
                     f'args={ARGS:016x} argp={ARGP:016x}',
                     f'result={RETURNED}'], lines


def convert(relinker, case, platform_args, name, module):
    modules = case / 'sce_module'
    modules.mkdir(parents=True)
    (modules / name).write_bytes(module)
    source = case / 'input.elf'
    source.write_bytes(main_fixture())
    output = case / ('output.exe' if platform_args else 'output.elf')
    result = subprocess.run([str(relinker), *platform_args,
                             '--skip-syscall-check', str(source), str(output)],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (result.stdout, result.stderr)
    return case / 'app0' / 'sce_module' / (name + '.guest.prx')


def main():
    if sys.argv[1] == '--load':
        import ctypes
        host = ctypes.CDLL(sys.argv[2], mode=ctypes.RTLD_GLOBAL if sys.platform == 'darwin' else ctypes.DEFAULT_MODE)
        host.SetPendingArgs.argtypes = [ctypes.c_size_t, ctypes.c_void_p]
        host.SetPendingArgs(ARGS, ARGP)
        ctypes.CDLL(sys.argv[3])
        return
    relinker = Path(sys.argv[1]).resolve()
    host = Path(sys.argv[2]).resolve()
    linux = sys.platform == 'linux' and os.uname().machine == 'x86_64'
    with tempfile.TemporaryDirectory(prefix='anyps5-guest-preinit-once-') as directory:
        work = Path(directory)
        name = 'preinit.prx'
        converted = convert(relinker, work / 'linux', [], name, recording_module())
        if linux:
            libraries = work / 'libs'
            libraries.mkdir()
            shutil.copyfile(host, libraries / 'libc.prx')
            shutil.copyfile(host, libraries / 'libkernel.prx')
            load_and_record(work, libraries / 'libc.prx', converted)
        mach_converted = convert(relinker, work / 'macos', ['--macos'], name, recording_module())
        preinit_slots = {IMAGE_BASE + LOAD_RVA + PREINIT_SLOTS, IMAGE_BASE + LOAD_RVA + PREINIT_SLOTS + 8}
        targets = set(macos_call_targets(mach_converted.read_bytes()))
        assert not (targets & preinit_slots), (sorted(targets), sorted(preinit_slots))
        print('Guest preinit single-run test passed' if linux else 'Guest preinit single-run test passed: native execution skipped')


if __name__ == '__main__':
    main()
