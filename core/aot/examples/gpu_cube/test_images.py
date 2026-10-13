import struct
import tempfile
import unittest
from pathlib import Path

from images import compare, compare_frames, read_bmp


def write_bmp(path, rows, bits=32, top_down=True):
    width, height = len(rows[0]), len(rows)
    stride = ((width * bits + 31) // 32) * 4
    pixels = bytearray()
    for row in rows if top_down else reversed(rows):
        packed = b''.join(bytes(color) + (b'\xff' if bits == 32 else b'') for color in row)
        pixels.extend(packed + b'\0' * (stride - len(packed)))
    header = b'BM' + struct.pack('<IHHI', 54 + len(pixels), 0, 0, 54)
    header += struct.pack('<IiiHHIIiiII', 40, width, -height if top_down else height, 1, bits, 0, len(pixels), 0, 0, 0, 0)
    path.write_bytes(header + pixels)


class Images(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.rows = [[(10, 20, 30), (40, 50, 60), (70, 80, 90)], [(1, 2, 3), (4, 5, 6), (7, 8, 9)]]

    def test_row_order_and_padding(self):
        a, b = self.folder / 'a.bmp', self.folder / 'b.bmp'
        write_bmp(a, self.rows, 24, False)
        write_bmp(b, self.rows)
        self.assertEqual(read_bmp(a), read_bmp(b))
        self.assertEqual(compare(a, b)['different_pixels'], 0)

    def test_one_pixel_difference_fails_exact_comparison(self):
        a, b = self.folder / 'a.bmp', self.folder / 'b.bmp'
        write_bmp(a, self.rows)
        self.rows[1][2] = (7, 8, 10)
        write_bmp(b, self.rows)
        result = compare(a, b)
        self.assertEqual(result['different_pixels'], 1)
        self.assertEqual(result['first_differences'], [[2, 1]])

    def test_truncated_or_oversized_capture_rejects(self):
        path = self.folder / 'a.bmp'
        write_bmp(path, self.rows)
        good = path.read_bytes()
        path.write_bytes(good[:-1])
        with self.assertRaises(ValueError):
            read_bmp(path)
        raw = bytearray(good)
        struct.pack_into('<i', raw, 18, 8193)
        path.write_bytes(raw)
        with self.assertRaises(ValueError):
            read_bmp(path)

    def test_missing_extra_blank_and_frozen_captures_reject(self):
        a, b = self.folder / 'a', self.folder / 'b'
        a.mkdir(); b.mkdir()
        with self.assertRaises(ValueError):
            compare_frames(a, b, 2)
        for folder in (a, b):
            for index in range(2):
                write_bmp(folder / f'frame_{index:03d}.bmp', [[(0, 0, 0)]])
        self.assertFalse(compare_frames(a, b, 2)['passed'])
        for folder in (a, b):
            for index in range(2):
                write_bmp(folder / f'frame_{index:03d}.bmp', self.rows)
        self.assertFalse(compare_frames(a, b, 2)['passed'])
        self.rows[0][0] = (20, 10, 30)
        for folder in (a, b):
            write_bmp(folder / 'frame_001.bmp', self.rows)
        self.assertTrue(compare_frames(a, b, 2)['passed'])
        write_bmp(a / 'frame_002.bmp', self.rows)
        with self.assertRaises(ValueError):
            compare_frames(a, b, 2)


if __name__ == '__main__':
    unittest.main()
