# Qualified dependency inventory

`dependency_compatibility.py` produces a static JSON report for every import in an explicit supplied consumer set. It reads saved JSON metadata and frozen CPU resolver evidence only. It neither reads guest binaries nor executes a title, scans an image, mounts a filesystem, builds an engine, or changes a package.

```sh
python3 tools/dependency_compatibility.py \
  --consumer-metadata /private/main.metadata.json \
  --consumer-metadata /private/dependency.metadata.json \
  --host-evidence /private/frozen-host-evidence.json \
  --output /private/new-dependency-report.json
```

Generate the host evidence first from the audited immutable source snapshot:

```sh
python3 tools/extract_cpu_host_evidence.py \
  --source-root /path/to/AnyPS5 \
  --source-commit 042b41f4c9a03f65c853872f7f9dd189736225f2 \
  --modern-tcg unknown \
  --output /private/new-frozen-host-evidence.json
```

The extraction adapter reads Git objects at the explicit commit and verifies audited source hashes and registration counts. It accepts only the pinned audited revision. A later source revision requires a new source audit and adapter update; source code being present in a moving worktree does not qualify it. `--modern-tcg` is an explicit analysis profile: use `unknown` when the package profile is unverified. Conditional thread entries remain unknown in that profile, and `disabled` entries are omitted from effective resolution.

Argument order is significant: the first consumer is the main executable; subsequent consumers are the entire supplied guest dependency set. Supply every available consumer, including dependencies that themselves import other modules. The output parent must exist and the report must be outside the tool worktree and frozen source repository. The tool rejects existing outputs and input overwrites, and atomically publishes the report with private file permissions. Paths, source identities and imports can be sensitive: keep reports and real metadata outside Git.

The report records the tool hash, exact metadata hashes, guest source hashes and sizes, and host evidence source commit. `consumers` contains separate `needed_files`, `import_modules` and `imports` arrays. Each import preserves NID, library/version, module/major/minor, type, size, binding, visibility and section. `summary` counts symbol classifications; filename and declared module rows have their own classifications and provider cardinalities.

| Classification | Meaning for an import |
| --- | --- |
| `supplied_guest` | Exactly one visible supplied guest function export matches scope, type, section and size constraints, without a competing host declaration. This is static metadata acceptance only. |
| `qualified_host` | An effective CPU host declaration matches module/library versions, and actual qualified resolver evidence matches NID and type, with adequate declared object size. |
| `missing_provider` | No exact guest export or host scope exists; or a certified complete CPU resolver inventory lacks the requested NID. |
| `mismatch` | Available evidence conflicts in scope/version, type or storage size; uses an unsupported import/absolute export; has ambiguous providers; or requests host TLS. |
| `unknown` | Only a host declaration or unqualified resolver evidence exists, the resolver inventory is incomplete, or guest object readable storage/TLS backing is unavailable. |

A matching filename only proves the supplied file metadata is present. Host filenames and declared modules never prove symbol support. `import_modules.provider_count` enforces the frozen resolver's one-provider module/version rule. The report preserves provider references and reasons rather than translating NIDs to guessed function names. Hidden/internal guest exports are ineligible. Any visible exact-scope guest export with a wrong type is a mismatch, even if another candidate has the right type.

The host evidence `effective_host_policy` records `Main.cpp` selection behavior: matching supplied dependency guest module/version declarations suppress host modules; the main executable does not suppress them. A `libkernel.prx` request selects that alias when documented by the policy, while simultaneous `.prx` and `.sprx` requirements are reported as a graph mismatch. Missing policy evidence is reported as an unknown graph setup. Resolver entries with explicit distinct `resolver_order` use the earliest matching entry; conditional or unqualified earlier entries remain unknown. Entries without proven precedence remain ambiguous.

`unknown_provider_closure` lists each absent needed filename and its consumers. Nothing in a consumer reveals the absent provider's own dependencies, so those closures remain unknown. Runtime profile acceptance, mapped executable/readable storage, CRT certification, TLS layout, relocations and service ABI semantics are not established by this report. Even a fully classified function inventory is not evidence that a game loads or runs.

Consumer JSON uses the saved parser fields `path`, `size`, `sha256`, `needed_files`, `import_modules`, `import_libraries`, `export_modules`, `export_libraries`, `imports` and `exports`. Module declarations contain `name` and packed 16-bit `version`; libraries contain `name` and `version`. Symbol entries contain the fields listed above. Malformed or incomplete symbol identities fail closed instead of qualifying support. Storage fields absent from the saved parser are kept unknown rather than reconstructed from private binaries.

Host evidence schema version 1 contains:

- `source.commit`: exact 40-character frozen Git SHA, with source references/hashes from extraction.
- `modules`: `filename`, `module`, `module_major`, `module_minor`, explicit `libraries` (`library`/`library_version`, or `name`/`version`), and source evidence.
- `exports`: complete NID/library/module scope, `type`, `size`, `qualification` (`qualified` or `unknown`), and resolver evidence. Qualified entries require type, size and evidence. Optional `resolver_order` proves precedence.
- `effective_host_policy`: documented booleans `suppress_matching_guest_modules` and `kernel_filename_alias`, plus source evidence.
- Optional `resolver_inventory_complete: true`: certification that absent NIDs are absent from the complete audited CPU resolver set. Without it, a declared host scope lacking a NID is unknown.
- `unknowns`: extraction limitations and unproved contracts, carried into the report.

Malformed inputs return CLI status 2 without publishing a report. The tool writes a short receipt to stdout containing the output path, SHA-256 and classification counts. Reports are repeatable for the same exact paths, tool and input bytes; a rerun requires a fresh output filename.
