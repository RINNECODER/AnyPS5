import hashlib
import struct
from pathlib import Path


def read_bmp(path):
    data = Path(path).read_bytes()
    if len(data) < 54 or data[:2] != b'BM':
        raise ValueError('invalid BMP header')
    declared, offset = struct.unpack_from('<IxxxxI', data, 2)
    header, width, height, planes, bits, compression = struct.unpack_from('<IiiHHI', data, 14)
    if declared != len(data) or header < 40 or offset < 14 + header or width <= 0 or height == 0 or planes != 1 or bits not in (24, 32) or compression != 0:
        raise ValueError('unsupported BMP layout')
    if width > 8192 or abs(height) > 8192:
        raise ValueError('BMP exceeds comparison bounds')
    stride = ((width * bits + 31) // 32) * 4
    if len(data) - offset != stride * abs(height):
        raise ValueError('truncated BMP pixels')
    result = bytearray(width * abs(height) * 3)
    for y in range(abs(height)):
        source_y = y if height < 0 else height - 1 - y
        row = data[offset + source_y * stride:offset + (source_y + 1) * stride]
        for x in range(width):
            start = x * (bits // 8)
            result[(y * width + x) * 3:(y * width + x + 1) * 3] = row[start:start + 3]
    return width, abs(height), bytes(result)


def compare(left, right):
    width, height, a = read_bmp(left)
    other_width, other_height, b = read_bmp(right)
    if (width, height) != (other_width, other_height):
        raise ValueError('capture dimensions differ')
    changed = [i // 3 for i in range(0, len(a), 3) if a[i:i + 3] != b[i:i + 3]]
    return {'dimensions': [width, height], 'different_pixels': len(changed),
            'first_differences': [[i % width, i // width] for i in changed[:16]],
            'aot_pixels_sha256': hashlib.sha256(a).hexdigest(),
            'reference_pixels_sha256': hashlib.sha256(b).hexdigest(),
            'aot_colors': len({a[i:i + 3] for i in range(0, len(a), 3)}),
            'reference_colors': len({b[i:i + 3] for i in range(0, len(b), 3)})}


def compare_frames(left, right, count=8):
    expected = [f'frame_{index:03d}.bmp' for index in range(count)]
    for folder in (left, right):
        if sorted(p.name for p in Path(folder).glob('frame_*.bmp')) != expected:
            raise ValueError('capture set is incomplete or contains unexpected frames')
    frames = [dict(frame=name, **compare(Path(left) / name, Path(right) / name)) for name in expected]
    nonblank = all(f['aot_colors'] >= 2 and f['reference_colors'] >= 2 for f in frames)
    moving = len({f['aot_pixels_sha256'] for f in frames}) >= min(4, count)
    return {'frames': frames, 'nonblank': nonblank, 'moving': moving,
            'passed': nonblank and moving and all(f['different_pixels'] == 0 for f in frames),
            'comparison': 'Exact RGB equality; alpha is not compared'}
