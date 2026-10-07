"""Compiled public CLI controls for lossless, fail-closed static SCE metadata.

Contract: real linked x86-64 records retain scoped NID/type/binding/visibility/
section/size and source identities through binary -> JSON -> qualified report;
plaintext SELF produces the same ABI evidence, while malformed or ambiguous
inputs cannot publish partial evidence or overwrite an input/existing output.
Regression: dropping symbol type misqualifies WrongType01, guessing object/TLS
storage loses unknowns, an ID alias binds a symbol to another library, or a
truncated/overflowing table yields apparently complete JSON. Existing report
controls begin with synthetic JSON and cannot detect these producer failures.
The public CLIs are the boundary; no production test-only seam is introduced.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
SCRIPT = TOOLS / 'inspect_sce_metadata.py'
SOURCE_COMMIT = '042b41f4c9a03f65c853872f7f9dd189736225f2'
BUILDER_PATH = TOOLS / 'tests/fixtures/sce_metadata/build_fixture.py'
spec = importlib.util.spec_from_file_location('public_sce_fixture', BUILDER_PATH)
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
sys.path.insert(0, str(TOOLS))
from prepare_diagnostic_engine import Lease


class RichMetadataContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='sce-metadata-controls-')
        cls.root = Path(cls.temporary.name)
        try:
            with Lease(['tcg-build'], owner='tools04-public-fixture-controls'):
                cls.commands = builder.compile_fixture(cls.root)
        except BaseException:
            cls.temporary.cleanup()
            raise
        cls.main = cls.root / 'main.bin'
        cls.provider = cls.root / 'fixture.prx'
        cls.layout = json.loads((cls.root / 'consumer.layout.json').read_text())

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def invoke(self, source, output=None):
        output = output or self.root / (source.name + '.metadata-' + str(len(list(self.root.glob('*.json')))) + '.json')
        original = source.read_bytes()
        result = subprocess.run([sys.executable, str(SCRIPT), '--input', str(source),
                                 '--output', str(output)], capture_output=True, text=True, timeout=20)
        self.assertEqual(source.read_bytes(), original, 'producer altered its source')
        return result, output

    def inspect(self, source):
        result, output = self.invoke(source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
        metadata = json.loads(output.read_text())
        self.assertEqual(metadata['path'], str(source.resolve()))
        self.assertEqual(metadata['sha256'], hashlib.sha256(source.read_bytes()).hexdigest())
        self.assertEqual(metadata['size'], source.stat().st_size)
        return metadata, output

    def test_compiled_binary_rich_metadata_and_pinned_host_report(self):
        consumer, consumer_path = self.inspect(self.main)
        provider, provider_path = self.inspect(self.provider)
        self.assertEqual(consumer['needed_files'], ['fixture.prx', 'libSceCommonDialog.prx', 'missing.prx'])
        self.assertEqual(consumer['import_modules'], [
            {'name': 'libSceCommonDialog', 'id': 1, 'version': 257},
            {'name': 'fixtureModule', 'id': 2, 'version': 257}])
        self.assertEqual(consumer['import_libraries'], [
            {'name': 'libSceCommonDialog', 'id': 1, 'version': 1},
            {'name': 'fixtureLibrary', 'id': 2, 'version': 1}])
        self.assertEqual(consumer['import_attributes'], [{'library_id': 2, 'attributes': 0x12345}])
        self.assertEqual(provider['export_attributes'], [{'library_id': 2, 'attributes': 0x23456}])
        self.assertEqual([(row['tag'], row['value'], row['qualification'])
                          for row in consumer['module_attributes']],
                         [(0x61000011, 0x123456789abc, 'unknown')])
        imports = {row['nid']: row for row in consumer['imports']}
        self.assertEqual(set(imports), {'uoUpLGNkygk', 'GuestFun001', 'AbsentFun01', 'WrongType01', 'GuestObj001', 'GuestTls001'})
        for nid, symbol_type in [('uoUpLGNkygk', 2), ('GuestFun001', 2), ('AbsentFun01', 2),
                                 ('WrongType01', 2), ('GuestObj001', 1), ('GuestTls001', 6)]:
            row = imports[nid]
            self.assertEqual((row['type'], row['binding'], row['visibility'], row['section'], row['size']),
                             (symbol_type, 1, 0, 0, 0))
            self.assertEqual((row['module_major'], row['module_minor'], row['library_version']), (1, 1, 1))
        self.assertEqual((imports['uoUpLGNkygk']['module'], imports['uoUpLGNkygk']['library']),
                         ('libSceCommonDialog', 'libSceCommonDialog'))
        exports = {row['nid']: row for row in provider['exports']}
        self.assertEqual(set(exports), {'GuestFun001', 'WrongType01', 'GuestObj001', 'GuestTls001'})
        self.assertEqual((exports['WrongType01']['type'], exports['WrongType01']['size']), (1, 8))
        self.assertEqual((exports['GuestObj001']['type'], exports['GuestObj001']['size']), (1, 8))
        self.assertEqual((exports['GuestTls001']['type'], exports['GuestTls001']['size']), (6, 8))
        self.assertTrue(all(row['section'] > 0 for row in exports.values()))
        self.assertEqual((exports['GuestFun001']['type'], exports['GuestFun001']['binding'],
                          exports['GuestFun001']['visibility'], exports['GuestFun001']['size']), (2, 2, 3, 6))
        self.assertGreater(exports['GuestFun001']['value'], 0)
        self.assertFalse(any('storage' in key or 'backing' in key for row in exports.values() for key in row))
        host = self.root / 'pinned-host.json'
        extracted = subprocess.run([sys.executable, str(TOOLS / 'extract_cpu_host_evidence.py'),
            '--source-root', str(TOOLS.parent), '--source-commit', SOURCE_COMMIT, '--modern-tcg', 'unknown',
            '--output', str(host)], capture_output=True, text=True, timeout=20)
        self.assertEqual(extracted.returncode, 0, extracted.stdout + extracted.stderr)
        evidence = json.loads(host.read_text())
        self.assertEqual(evidence['source']['commit'], SOURCE_COMMIT)
        report_path = self.root / 'pipeline-report.json'
        command = [sys.executable, str(TOOLS / 'dependency_compatibility.py'),
                   '--consumer-metadata', str(consumer_path), '--consumer-metadata', str(provider_path),
                   '--host-evidence', str(host), '--output', str(report_path)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(report_path.read_text())
        self.assertEqual({row['nid']: row['classification'] for row in report['consumers'][0]['imports']}, {
            'uoUpLGNkygk': 'qualified_host', 'GuestFun001': 'supplied_guest', 'AbsentFun01': 'missing_provider',
            'WrongType01': 'mismatch', 'GuestObj001': 'unknown', 'GuestTls001': 'unknown'})
        self.assertEqual(report['summary'], {'qualified_host': 1, 'supplied_guest': 1,
                                            'missing_provider': 1, 'mismatch': 1, 'unknown': 2})
        self.assertEqual(report['runtime_compatibility'], 'not_established')
        self.assertEqual([(row['filename'], row['required_by']) for row in report['unknown_provider_closure']],
                         [('missing.prx', ['main.bin'])])
        # Repeat the real producer with identical source/path bytes, fresh output.
        _, repeat = self.inspect(self.main)
        self.assertEqual(consumer_path.read_bytes(), repeat.read_bytes())

        # Source filename compatibility is distinct from SCE identity syntax:
        # '#' is valid in the report's basename contract and must stay valid.
        allowed = self.root / 'public#dump.bin'
        encoded = self.main.read_bytes()
        self.assertEqual(encoded.count(b'missing.prx\0'), 1)
        allowed.write_bytes(encoded.replace(b'missing.prx\0', b'miss#ng.prx\0'))
        _, allowed_path = self.inspect(allowed)
        allowed_report = self.root / 'allowed-source-name-report.json'
        command[command.index(str(consumer_path))] = str(allowed_path)
        command[command.index(str(report_path))] = str(allowed_report)
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        allowed_data = json.loads(allowed_report.read_text())
        self.assertEqual(allowed_data['summary'], report['summary'])
        self.assertEqual([row['filename'] for row in allowed_data['unknown_provider_closure']], ['miss#ng.prx'])

    def test_standard_linked_elf_and_plaintext_self_preserve_typed_evidence(self):
        # Ordinary ELF carries public names instead of SCE scoped identities.
        standard, _ = self.inspect(self.root / 'provider.elf')
        symbols = {row['name']: row for row in standard['symbols']}
        self.assertEqual((symbols['guest_function']['type'], symbols['guest_function']['size']), (2, 6))
        self.assertEqual((symbols['guest_object']['type'], symbols['guest_object']['size']), (1, 8))
        self.assertEqual((symbols['guest_tls']['type'], symbols['guest_tls']['size']), (6, 8))
        plaintext = self.root / 'public-plain.self'
        plaintext.write_bytes(builder.plain_self(self.main.read_bytes()))
        wrapped, _ = self.inspect(plaintext)
        raw, _ = self.inspect(self.main)
        self.assertEqual(wrapped['container'], 'plain_self')
        for key in ('needed_files', 'import_modules', 'import_libraries', 'import_attributes', 'imports', 'exports'):
            self.assertEqual(wrapped[key], raw[key], key)
        second_magic = bytearray(plaintext.read_bytes())
        second_magic[:4] = bytes.fromhex('5414f5ee')
        other = self.root / 'public-second-magic.self'
        other.write_bytes(second_magic)
        alternative, _ = self.inspect(other)
        self.assertEqual(alternative['imports'], raw['imports'])

        # Contract: ignored ELF physical addresses cannot reject valid metadata.
        # A parser-wide overflow check is a credible regression; other controls
        # cover consumed offsets/virtual addresses, not this unused sentinel.
        sentinel = bytearray(self.main.read_bytes())
        phoff, = struct.unpack_from('<Q', sentinel, 32)
        phcount, = struct.unpack_from('<H', sentinel, 56)
        index = next(i for i in range(phcount)
                     if struct.unpack_from('<I', sentinel, phoff + i * 56)[0] == 1)
        struct.pack_into('<Q', sentinel, phoff + index * 56 + 24, (1 << 64) - 1)
        unused_physical = self.root / 'ignored-physical-address.bin'
        unused_physical.write_bytes(sentinel)
        unused_metadata, _ = self.inspect(unused_physical)
        self.assertEqual(unused_metadata['imports'], raw['imports'])
        wrapped_physical = self.root / 'ignored-physical-address.self'
        wrapped_physical.write_bytes(builder.plain_self(sentinel))
        wrapped_metadata, _ = self.inspect(wrapped_physical)
        self.assertEqual(wrapped_metadata['imports'], raw['imports'])

    def test_malformed_overflow_ambiguity_and_encrypted_self_fail_without_output(self):
        original = self.main.read_bytes()
        layout = self.layout
        changes = {}
        changes['truncated-program-table'] = (original[:-1], 'invalid program header table')
        bad = bytearray(original)
        struct.pack_into('<Q', bad, 32, (1 << 64) - 8)
        changes['overflow-program-offset'] = (bad, 'invalid program header table')
        bad = bytearray(original)
        struct.pack_into('<Q', bad, layout['tag_offsets'][str(0x61000037)] + 8, (1 << 64) - 1)
        changes['overflow-string-size'] = (bad, 'invalid dynamic string table size')
        bad = bytearray(original)
        last = layout['tag_offsets']['0']
        struct.pack_into('<QQ', bad, last, 1, 0)
        changes['unterminated-dynamic'] = (bad, 'unterminated dynamic segment')
        bad = bytearray(original)
        # Second module entry aliases the first ID with a different name.
        first_module = layout['dynamic_offset'] + 2 * 16
        value, = struct.unpack_from('<Q', bad, first_module + 8)
        struct.pack_into('<Q', bad, first_module + 8, (value & ((1 << 48) - 1)) | (1 << 48))
        changes['ambiguous-module-id'] = (bad, 'conflicting module or library identity')
        bad = bytearray(original)
        struct.pack_into('<Q', bad, layout['tag_offsets'][str(0x6100003f)] + 8, 25)
        changes['partial-symbol-record'] = (bad, 'invalid dynamic symbol table size')
        for label, mask, reason in [('encrypted-self', 2, 'encrypted SELF segments unsupported'),
                                   ('compressed-self', 8, 'compressed SELF segments unsupported'),
                                   ('unknown-self-property', 1 << 63, 'unsupported SELF segment properties')]:
            bad = bytearray(builder.plain_self(original))
            props, = struct.unpack_from('<Q', bad, 32)
            struct.pack_into('<Q', bad, 32, props | mask)
            changes[label] = (bad, reason)
        bad = bytearray(original)
        struct.pack_into('<Q', bad, layout['tag_offsets'][str(0x61000019)] + 8, (3 << 48) | 0x12345)
        changes['dangling-library-attribute'] = (bad, 'attribute has no declared library identity')
        bad = bytearray(original)
        struct.pack_into('<QQ', bad, layout['dynamic_offset'] + 4 * 16, 0x61000019, (2 << 48) | 0x98765)
        changes['conflicting-library-attribute'] = (bad, 'conflicting library attributes')
        bad = bytearray(original)
        struct.pack_into('<QQ', bad, layout['dynamic_offset'] + 4 * 16, 10, layout['string_size'])
        changes['ambiguous-standard-sce-tag'] = (bad, 'missing or ambiguous standard/SCE dynamic tag')
        bad = bytearray(original)
        struct.pack_into('<Q', bad, layout['symbol_offset'] + 16, 1)
        changes['nonzero-null-symbol'] = (bad, 'invalid null dynamic symbol')
        bad = bytearray(original)
        symbol = layout['symbol_offset'] + 24 * layout['symbols']['GuestObj001']
        struct.pack_into('<QQ', bad, symbol + 8, (1 << 64) - 1, 8)
        changes['overflow-symbol-value'] = (bad, 'symbol value/size overflows uint64')
        bad = bytearray((self.root / 'provider.elf').read_bytes())
        phoff, = struct.unpack_from('<Q', bad, 32)
        phcount, = struct.unpack_from('<H', bad, 56)
        # Replace non-file-backed GNU_STACK with a duplicate load segment.
        stack = next(i for i in range(phcount) if struct.unpack_from('<I', bad, phoff + i * 56)[0] == 0x6474e551)
        load = next(i for i in range(phcount) if struct.unpack_from('<I', bad, phoff + i * 56)[0] == 1)
        bad[phoff + stack * 56:phoff + (stack + 1) * 56] = bad[phoff + load * 56:phoff + (load + 1) * 56]
        changes['ambiguous-standard-virtual-backing'] = (bad, 'dynamic virtual address has missing or ambiguous file backing')
        for label, (content, reason) in changes.items():
            with self.subTest(control=label):
                source = self.root / (label + '.bin')
                source.write_bytes(content)
                output = self.root / (label + '.json')
                result, _ = self.invoke(source, output)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertFalse(output.exists(), 'invalid input published partial JSON')
                self.assertIn(reason, result.stderr, 'negative control failed for an unrelated guard')

    def test_input_existing_output_symlink_and_private_atomic_publication(self):
        # Contract: every published source path is consumable by the report.
        # A valid POSIX basename can violate its schema; ordinary fixture names
        # and existing alias controls do not detect that compatibility gap.
        for character in (':', '\\', '\r', '\n'):
            with self.subTest(source_character=repr(character)):
                invalid_source = self.root / ('public' + character + 'dump.bin')
                invalid_source.write_bytes(self.main.read_bytes())
                result, output = self.invoke(invalid_source)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn('source filename incompatible with dependency report', result.stderr)
                self.assertFalse(output.exists())
        content = b'existing output must survive\n'
        existing = self.root / 'existing-output.json'
        existing.write_bytes(content)
        for output in (self.main, existing):
            result, _ = self.invoke(self.main, output)
            self.assertEqual(result.returncode, 2)
        self.assertEqual(existing.read_bytes(), content)
        symlink = self.root / 'alias-output.json'
        symlink.symlink_to(self.main)
        result, _ = self.invoke(self.main, symlink)
        self.assertEqual(result.returncode, 2)
        self.assertTrue(symlink.is_symlink())
        hardlink = self.root / 'hardlink-output.json'
        os.link(self.main, hardlink)
        result, _ = self.invoke(self.main, hardlink)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(hardlink.read_bytes(), self.main.read_bytes())
        # Public fake Git root exercises output containment without repo mutation.
        git_root = self.root / 'fake-git-root'
        git_root.mkdir()
        (git_root / '.git').mkdir()
        blocked = git_root / 'output.json'
        result, _ = self.invoke(self.main, blocked)
        self.assertEqual(result.returncode, 2)
        self.assertIn('outside Git repositories', result.stderr)
        self.assertFalse(blocked.exists())
        before = {path.name for path in self.root.iterdir()}
        self.inspect(self.main)
        self.assertFalse(any(name.startswith('.sce-metadata-') for name in
                             {path.name for path in self.root.iterdir()} - before))


if __name__ == '__main__':
    unittest.main()
