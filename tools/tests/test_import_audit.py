"""Primary CLI owner: contract/regression/coverage-gap/no-seam gate in worker evidence.

Conserve all registry references; distinguish static name presence from ABI;
follow PE ordinals to nonzero bounded addresses; malformed inputs fail closed;
never overwrite inputs. Existing typed SCE report tests do not parse relinker
registry + patched ELF/PE bytes. Independent binary fixtures below cross the
actual subprocess CLI, without mocks or a production test seam.

ELF/PE fixture structure adapted from abderahmane-ai (#1461, 9e6a6004a5e8a251af8e19eab66b1ac3cad71bab)
and yahya308 (#1800, fd50daeb240ddb766690de6a077bb0a383c776a3).
GNU GPL version 2; see repository LICENSE.
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(os.environ.get('IMPORT_AUDIT_SCRIPT', Path(__file__).resolve().parents[1] / 'import_audit.py'))
A, B, C = 'AAAAAAAAAAA', 'BBBBBBBBBBB', 'CCCCCCCCCCC'
MESSAGE_NID, MESSAGE_NAME = 'wTpfglkmv34', 'sceMsgDialogProgressBarSetValue'


def elf(symbols):
    """ELF64 ET_DYN with .text, .dynstr and .dynsym; tuples name/shndx/info/visibility/value."""
    strings = b'\0'
    table = bytes(24)
    for name, section, info, visibility, value in symbols:
        table += struct.pack('<IBBHQQ', len(strings), info, visibility, section, value, 1)
        strings += name.encode() + b'\0'
    table_offset = (65 + len(strings) + 7) & ~7
    section_offset = (table_offset + len(table) + 7) & ~7
    image = bytearray(section_offset + 4 * 64)
    image[64] = 0xc3
    image[65:65 + len(strings)] = strings
    image[table_offset:table_offset + len(table)] = table
    headers = [bytes(64), struct.pack('<IIQQQQIIQQ', 0, 1, 6, 0x1000, 64, 1, 0, 0, 1, 0),
               struct.pack('<IIQQQQIIQQ', 0, 3, 2, 0, 65, len(strings), 0, 0, 1, 0),
               struct.pack('<IIQQQQIIQQ', 0, 11, 2, 0, table_offset, len(table), 2, 1, 8, 24)]
    image[section_offset:] = b''.join(headers)
    image[:64] = struct.pack('<16sHHIQQQIHHHHHH', b'\x7fELF\x02\x01\x01' + bytes(9),
                             3, 0x3e, 1, 0, 0, section_offset, 0, 64, 0, 0, 64, 4, 0)
    return image


def exports(*names):
    return elf([(name, 1, 0x12, 0, 0x1000) for name in names])


def pe(names=(A,), addresses=None, ordinals=None, magic=0x20b):
    """PE32/PE32+ .edata: EAT, name-pointer table, ordinal table and NUL strings."""
    addresses = addresses if addresses is not None else [0x1000 + 40 + 10 * len(names) + sum(len(n) + 1 for n in names)] * len(names)
    ordinals = ordinals if ordinals is not None else list(range(len(names)))
    names_start = 40 + 4 * len(addresses) + 6 * len(names)
    strings = b''
    pointers = []
    for name in names:
        pointers.append(0x1000 + names_start + len(strings))
        strings += name.encode() + b'\0'
    raw = struct.pack('<IIHHIIIIIII', 0, 0, 0, 0, 0, 17, len(addresses), len(names),
                      0x1028, 0x1028 + 4 * len(addresses), 0x1028 + 4 * len(addresses) + 4 * len(names))
    raw += b''.join(struct.pack('<I', a) for a in addresses)
    raw += b''.join(struct.pack('<I', p) for p in pointers)
    raw += b''.join(struct.pack('<H', o) for o in ordinals) + strings
    export_size = len(raw)
    raw += b'\xc3'
    directory = 112 if magic == 0x20b else 96
    optional_size = directory + 128
    image = bytearray(0x200)
    image[:2] = b'MZ'
    struct.pack_into('<I', image, 0x3c, 0x40)
    image[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', image, 0x44, 0x8664 if magic == 0x20b else 0x14c, 1, 0, 0, 0, optional_size, 0x2022)
    struct.pack_into('<H', image, 0x58, magic)
    struct.pack_into('<I', image, 0x58 + directory - 4, 16)
    struct.pack_into('<II', image, 0x58 + directory, 0x1000, export_size)
    struct.pack_into('<8sIIIIIIHHI', image, 0x58 + optional_size, b'.edata', len(raw), 0x1000, len(raw), 0x200, 0, 0, 0, 0, 0x40000040)
    return image + raw


class ImportAuditCLI(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='import-audit-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.libs, self.modules, self.source = [self.root / n for n in ('libs', 'modules', 'source')]
        for path in (self.libs, self.modules, self.source):
            path.mkdir()
        self.library = self.libs / 'libSceA.prx'
        self.library.write_bytes(exports(A))
        self.registry = self.root / 'registry.json'
        self.entries([(A, self.library.name)])
        self.output = self.root / 'audit.json'

    def entries(self, rows):
        self.registry.write_text(json.dumps([{'nid': nid, 'library': library,
            'relocationType': 'R_X86_64_GLOB_DAT', 'callSites': ['0xA0']} for nid, library in rows]), encoding='utf-8-sig')

    def run_cli(self, *extra, output=None):
        output = output or self.output
        inputs = list(self.root.rglob('*'))
        original = {p: p.read_bytes() for p in inputs if p.is_file()}
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.registry), *extra, '--libs', str(self.libs),
            '--modules', str(self.modules), '--source', str(self.source), '--json', str(output)],
            capture_output=True, text=True, timeout=15)
        for path, content in original.items():
            self.assertEqual(path.read_bytes(), content, f'input overwritten: {path}')
        return result

    def report(self, expected=0, *extra, **kwargs):
        result = self.run_cli(*extra, **kwargs)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertEqual(result.stderr, '')
        data = json.loads((kwargs.get('output') or self.output).read_text())
        self.assertEqual(data['runtime_abi_callability'], 'NOT_ESTABLISHED')
        self.assertIn('NOT_ESTABLISHED', result.stdout)
        return data

    def reject(self, diagnostic, **kwargs):
        result = self.run_cli(**kwargs)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(result.stdout, '', 'error emitted success report')
        self.assertIn(diagnostic, result.stderr)
        self.assertFalse(self.output.exists(), 'invalid input published report')

    def test_reference_inventory_and_source_hints_are_not_runtime_support(self):
        self.library.write_bytes(elf([(A, 1, 0x12, 0, 0x1000), (MESSAGE_NID, 1, 0x12, 0, 0x1000),
            ('UNDEFINED11', 0, 0x12, 0, 0), ('LOCALLOCAL1', 1, 0x02, 0, 0x1000),
            ('HIDDENHIDE1', 1, 0x12, 2, 0x1000), (B, 1, 0x22, 3, 0x1000), (C, 1, 0x12, 0, 0)]))
        (self.libs / 'libc.prx').write_bytes(exports('DDDDDDDDDDD'))
        (self.modules / 'unknown.prx').write_bytes(b'filename only, not a parsed provider')
        directory = self.source / 'libSceA'
        directory.mkdir()
        (directory / 'Export.cpp').write_text(f'int APS5_VABI {MESSAGE_NAME}(int v) {{ NotImplemented_nid_no_patch(__func__); return 0; }}\n'
            'APS5_EXPORT("BBBBBBBBBBB", sceUnknown_B);\nint APS5_VABI sceUnknown_B() { NotImplemented_nid_no_patch(__func__); return 0; }\n')
        self.entries([(A + '#A#B', self.library.name), (A, self.library.name), (MESSAGE_NID, self.library.name),
            (B, self.library.name), (C, self.library.name), ('UNDEFINED11', self.library.name),
            ('LOCALLOCAL1', self.library.name), ('HIDDENHIDE1', self.library.name),
            ('EEEEEEEEEEE', 'unknown.prx'), (A, 'other.prx'), ('DDDDDDDDDDD', 'other.prx')])
        second = self.root / 'second-registry.json'
        second.write_text(json.dumps([{'nid': A, 'library': self.library.name}]))
        data = self.report(1, str(second))
        self.assertEqual((data['references'], data['unique_imports']), (12, 10))
        self.assertEqual(sum(r['references'] for r in data['imports']), 12)
        self.assertEqual(data['references_by_class'], {'export-present': 7, 'export-missing': 4, 'export-unknown': 1})
        rows = {(r['nid'], r['library']): r for r in data['imports']}
        self.assertEqual(rows[A, self.library.name]['references'], 3)
        self.assertEqual(rows[MESSAGE_NID, self.library.name]['source_hint'], MESSAGE_NAME)
        self.assertEqual(rows[B, self.library.name]['source_hint'], 'sceUnknown_B')
        self.assertEqual(rows['EEEEEEEEEEE', 'unknown.prx']['providers'], [])
        self.assertEqual(rows['EEEEEEEEEEE', 'unknown.prx']['class'], 'export-unknown')
        self.assertTrue(rows['DDDDDDDDDDD', 'other.prx']['library_mismatch'])
        self.assertEqual(data['library_mismatch'], 2)
        self.assertEqual(data['scope_identity'], 'module/version identity unknown')
        self.assertRegex(data['audit_tool_commit'], r'^[0-9a-f]{40}$')

    def test_pe_zero_address_is_missing(self):
        self.library.write_bytes(pe(addresses=[0]))
        result = self.run_cli()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        row = json.loads(self.output.read_text())['imports'][0]
        self.assertEqual(row['class'], 'export-missing')
        self.assertEqual(row['providers'], [])

    def test_pe_ordinals_and_valid_nonzero_controls(self):
        for magic in (0x10b, 0x20b):
            for names, addresses, ordinals, expected in (
                ((A,), None, None, {A: 'export-present'}),
                ((A, B), [0x1000 + 40 + 20 + 24, 0], [1, 0], {A: 'export-missing', B: 'export-present'}),
                ((A, B), [0x1000 + 40 + 16 + 24], [0, 0], {A: 'export-present', B: 'export-present'})):
                with self.subTest(magic=magic, ordinals=ordinals):
                    self.library.write_bytes(pe(names, addresses, ordinals, magic))
                    self.entries([(n, self.library.name) for n in names])
                    self.output.unlink(missing_ok=True)
                    data = self.report(1 if 'export-missing' in expected.values() else 0)
                    self.assertEqual({r['nid']: r['class'] for r in data['imports']}, expected)

    def test_malformed_pe_tables_fail_without_partial_success(self):
        for case in ('ordinal', 'eat', 'names', 'ordinals', 'unterminated', 'zero-unterminated', 'directory-count', 'address', 'truncated'):
            with self.subTest(case=case):
                self.output.unlink(missing_ok=True)
                image = pe()
                diagnostic = 'libSceA.prx'
                if case == 'ordinal':
                    struct.pack_into('<H', image, 0x230, 1)
                    diagnostic = 'ordinal 1'
                elif case in ('eat', 'names', 'ordinals'):
                    struct.pack_into('<I', image, 0x200 + {'eat': 28, 'names': 32, 'ordinals': 36}[case], 0x12345)
                    diagnostic = 'RVA 0x12345'
                elif case in ('unterminated', 'zero-unterminated'):
                    image[-2:] = b'XX'
                    image += b'\0'  # terminator outside declared raw section, not acceptable
                    if case == 'zero-unterminated':
                        struct.pack_into('<I', image, 0x228, 0)
                    diagnostic = 'NUL-terminated'
                elif case == 'directory-count':
                    struct.pack_into('<I', image, 0x58 + 108, 0)
                    diagnostic = 'no export directory'
                elif case == 'address':
                    struct.pack_into('<I', image, 0x228, 0x12345)
                    diagnostic = 'RVA 0x12345'
                else:
                    image = image[:0x210]
                self.library.write_bytes(image)
                self.reject(diagnostic)

    def test_invalid_registry_elf_and_unsupported_formats_fail_closed(self):
        for image, diagnostic in ((b'\xcf\xfa\xed\xfe' + bytes(64), 'unsupported'), (b'unknown', 'unsupported'),
                                  (exports(A)[:-3], 'outside the file')):
            with self.subTest(diagnostic=diagnostic):
                self.library.write_bytes(image)
                self.reject(diagnostic)
        self.library.write_bytes(exports(A))
        for text, diagnostic in (('{}', 'JSON list'), ('[1', 'cannot read registry'),
                                  ('[{"nid":"short","library":"a"}]', 'invalid NID'),
                                  ('[{"nid":"AAAAAAAAAAA"}]', 'string fields')):
            with self.subTest(text=text):
                self.registry.write_text(text)
                self.reject(diagnostic)

    def test_empty_registry_and_ordinal_only_inventory(self):
        self.library.write_bytes(pe(names=(), addresses=[0x102c], ordinals=[]))
        self.entries([])
        data = self.report()
        self.assertEqual((data['references'], data['unique_imports'], data['imports']), (0, 0, []))
        self.output.unlink()
        self.entries([(A, self.library.name)])
        data = self.report(1)
        self.assertEqual(data['imports'][0]['class'], 'export-missing')

    def test_names_are_offline_and_unverified_names_stay_hints(self):
        self.entries([(MESSAGE_NID, self.library.name), (C, self.library.name)])
        names = self.root / 'names.csv'
        result = self.run_cli('--names', str(names))
        self.assertEqual((result.returncode, result.stdout), (2, ''))
        self.assertIn('never downloads', result.stderr)
        self.assertFalse(names.exists())
        self.assertFalse(self.output.exists())
        names.write_text(f'{MESSAGE_NID} {MESSAGE_NAME}\n{C} wrongname\n')
        result = self.run_cli('--names', str(names))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        rows = {r['nid']: r for r in json.loads(self.output.read_text())['imports']}
        self.assertEqual((rows[MESSAGE_NID]['name'], rows[MESSAGE_NID]['name_verified']), (MESSAGE_NAME, True))
        self.assertEqual((rows[C]['name'], rows[C]['name_verified']), ('wrongname', False))

    def test_output_refuses_input_trees_existing_files_and_aliases(self):
        source_file = self.source / 'Export.cpp'
        source_file.write_text('source bytes must survive')
        existing = self.root / 'existing.json'
        existing.write_bytes(b'existing report')
        symlink = self.root / 'symlink.json'
        symlink.symlink_to(self.registry)
        hardlink = self.root / 'hardlink.json'
        os.link(self.library, hardlink)
        for output in (self.registry, self.library, source_file, self.libs / 'new.json', self.modules / 'new.json',
                       self.source / 'new.json', existing, symlink, hardlink):
            with self.subTest(output=output):
                before = output.read_bytes() if output.exists() else None
                result = self.run_cli(output=output)
                self.assertEqual((result.returncode, result.stdout), (2, ''))
                self.assertIn('output', result.stderr.lower())
                if before is None:
                    self.assertFalse(output.exists())
                else:
                    self.assertEqual(output.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
