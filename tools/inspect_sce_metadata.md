# Public rich SCE metadata reader

`inspect_sce_metadata.py` reads one explicitly selected file and produces the rich consumer JSON accepted by `dependency_compatibility.py`. It uses Python's standard library. It does not execute, load, decrypt, authenticate, modify, scan for, or mount guest software.

```sh
python3 tools/inspect_sce_metadata.py \
  --input /private/consumer.bin \
  --output /private/new-consumer.metadata.json
python3 tools/dependency_compatibility.py \
  --consumer-metadata /private/new-consumer.metadata.json \
  --host-evidence /private/frozen-host-evidence.json \
  --output /private/new-dependency-report.json
```

Supply every available dependency separately to the producer and then to the report, with the main executable first. Generate frozen host evidence using the pinned extraction command in [dependency_compatibility.md](dependency_compatibility.md). This producer does not change that extractor's audited revision or qualify the current resolver.

The input must be a regular file, at most 256 MiB, with no input leaf symlink. Supported inputs are little-endian x86-64 ELF64 version 1: executable types `3`, `0xfe10`, `0xfe18`, OS ABI `0` or `9`, and ABI version at most `2`. Qualified imports/exports require SCE scoped dynamic metadata. Generic ELF symbol names are not converted to NIDs. Unscoped local/defined symbols remain in `symbols` without inferred qualified exports; an undefined external symbol requires the full SCE scope and otherwise fails. There must be exactly one nonempty terminated `PT_DYNAMIC` of at most 16 MiB and at most one `PT_SCE_DYNLIBDATA`. Tables may use standard virtual-address `DT_STRTAB`/`DT_SYMTAB` tags or their SCE offset aliases. Both forms for the same field are ambiguous and fail. Symbol count requires SCE `DT_SCE_SYMTABSZ` or a file-backed SysV `DT_HASH`; GNU hash alone is unsupported. Program headers are bounded to 1,024 and symbols to 1,048,576 entries.

Both SELF magic values accepted by the pinned public `Self.cpp` are supported only for its established plaintext profile. Every data entry requires a paired digest metadata entry with the correct block-count size. Encrypted, compressed, unknown-property, overlapping, missing-payload, malformed, or truncated containers fail. Digest contents are not authenticated. Reconstruction follows the public reader's omitted nonloadable NOTE and removed section-table normalization, recording those operations in `normalization_notes`. No reconstructed executable is written.

The output filename must be new, its parent must already exist, and it must be outside Git repositories. Existing files, symlinks and hardlinks cannot be overwritten, including aliases of the input. The reader writes a mode `0600` temporary file, flushes it, atomically links it to the new filename without clobbering, and removes the temporary name. Error status `2` leaves no published metadata. A successful receipt contains only the output path, output SHA-256 and import/export counts. Parsing errors do not print source paths, symbol scopes or payload bytes. The source is read with a bounded read and checked for changes during that read. Keep real metadata outside Git; it contains sensitive source paths and dependency identities.

Schema version 1 is deterministic for the same resolved input path, source bytes and producer bytes. JSON object keys are sorted; identity and attribute arrays are ordered by numeric ID, filenames are sorted, and dynamic tags and symbol arrays retain input order. Source provenance is the original container, including SELF, rather than a reconstructed ELF hash.

| Field | Contract |
| --- | --- |
| `path`, `size`, `sha256`, `container` | Resolved explicit input path, original byte count/SHA-256, `elf` or `plain_self`. SELF also records `declared_self_size`. |
| `elf_type`, `elf_os_abi`, `elf_abi_version`, `entry` | Raw ELF header values after supported SELF reconstruction. |
| `needed_files` | Complete `DT_NEEDED` filename declarations; duplicate declarations remain present. |
| `import_modules`, `export_modules`, `import_libraries`, `export_libraries` | Explicit `name`, 16-bit `id`, packed 16-bit `version`. Module major/minor are high/low bytes; no versions are inferred. Conflicting identities for the same ID fail. |
| `import_attributes`, `export_attributes` | Explicit `library_id` and low 48-bit `attributes` from library attribute tags. Referents must exist, conflicting attributes fail, equal repeats coalesce. Missing tags remain absent evidence, not zero-valued attributes. |
| `module_attributes` | Raw `DT_SCE_MODULE_ATTR` tag/value when present, with `qualification: unknown`. Pinned source does not establish its per-module scope or semantics; the reader does not invent imported/exported module attributes. Other unknown tags remain in `dynamic_tags`. |
| `imports`, `exports` | Every scoped external symbol includes `nid`, `scoped_name`, `library`, `library_id`, `library_version`, `module`, `module_id`, `module_major`, `module_minor`, `symbol_index`, `type`, `binding`, `visibility`, `section`, `value`, `size`. `scoped_name` preserves the raw name. Full `NID#library#module` scopes and declared qualifiers are required. |
| `symbols` | All dynamic symbols, including null/local/unscoped entries, with raw names and complete symbol attributes. Scoped entries also contain decoded identities. |
| `dynamic_tags`, `original_filenames` | All dynamic tag/value pairs through the null terminator; consistent original filename metadata. Unknown tags are retained without claiming runtime support. |
| `producer`, `normalization_notes`, `unknowns` | Immutable public reference commit/paths, producer SHA-256, SELF normalization, and explicit evidence limits. |

Null symbol index zero must have every raw field zero. Unsupported binding/visibility, unterminated strings, non-ASCII/control-character metadata, overflowing virtual or symbol ranges, missing or ambiguous file-backed translations, and conflicting symbol count tags fail. Typed symbols unsupported by the dependency resolver remain typed evidence for that report's `mismatch`/`unknown` decisions. The reader does not prove execution profile acceptance, readable object storage, TLS backing, relocations, CRT contracts, cryptographic authenticity, service ABI, or runtime behavior. In particular, no storage field is synthesized to turn guest objects or TLS into qualified providers.

The implementation derives SELF framing/reconstruction and SCE identity/tag decoding from the repository's GPL-2.0-only public source at immutable commit [`042b41f4c9a03f65c853872f7f9dd189736225f2`](https://github.com/RINNECODER/AnyPS5/tree/042b41f4c9a03f65c853872f7f9dd189736225f2):

- [`core/cpu/src/Self.cpp`](https://github.com/RINNECODER/AnyPS5/blob/042b41f4c9a03f65c853872f7f9dd189736225f2/core/cpu/src/Self.cpp), SHA-256 `c8ce1a1a8912e1721a2d0502e700e5a29feffb323e9b272591bcc3b95f3eba48`.
- [`core/cpu/src/SceElf.cpp`](https://github.com/RINNECODER/AnyPS5/blob/042b41f4c9a03f65c853872f7f9dd189736225f2/core/cpu/src/SceElf.cpp), SHA-256 `26dce2be331b0a872f4a0120200136873a3271f009850eb15e65c6ae8a62334d`.
- [ELF gABI symbol table](https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.symtab.html), for raw symbol fields and the all-zero null entry; [ELF gABI dynamic section](https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.dynamic.html), for standard string/symbol/hash tags.

The retained CPU02 producer was inspected statically for compatibility with its saved JSON contract; it was not executed or imported. Its private paths, data and top-level private-file workflow are not distributed. No standalone license was asserted for that untracked script. Unlike that reader, this public implementation preserves exported library attributes, rejects conflicting/dangling attributes, validates null/binding/visibility fields and ambiguous translations, and follows public SELF section/NOTE normalization. CPU02's existing saved outputs remain usable by the dependency report. The reduced native `--inspect-sce-json` still lacks the rich producer contract and must not be filled in with guessed fields.

Public compiled fixture generation and end-to-end/negative controls live under `tools/tests/fixtures/sce_metadata` and `tools/tests/test_inspect_sce_metadata.py`. They compile owned x86-64 fixture code and add explicit SCE metadata; no private binary fixture is distributed.
