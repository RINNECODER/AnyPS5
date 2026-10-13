# Native ARM64 engine releases

> Experimental. The main engine is the Rosetta 2 direct-execution engine
> ([docs/RELEASES.md](../RELEASES.md)). The native ARM64 engine follows behind it and publishes
> separately, on its own channels. Nothing here changes the Rosetta channels.

The native engine is the offline x86-64 to ARM64 compiler (`core/aot`, vendored from upstream, see
`core/aot/VENDORED.md`) plus the arm64 runtime a recompiled title links against (this fork's Metal
backend, `cmake/NativeAotGraphics.cmake`, option `ANYPS5_NATIVE_AOT`).

## What a native release contains

Code only. Package `anyps5-native-<version>-macos-arm64.zip`:

| Path | What |
|---|---|
| `bin/native_aot` | Launcher for the compiler (`python3`, relative to the package). |
| `compiler/*.py` | The AOT compiler. |
| `python/capstone`, `python/elftools`, `lib/libcapstone.dylib` | Its Python dependencies and the Capstone library (arm64). |
| `runtime/lib/*.a`, `runtime/include/NativeAotImage.hpp` | The arm64 runtime archives and header a recompiled title links against. `manifest.json` lists the link order and frameworks (`link`). |
| `share/AnyPS5Utilities.metallib`, `share/LICENSE`, `share/licenses/` | Metal utility shaders and licenses. |
| `manifest.json`, `SHA256SUMS` | `kind: native`, `arch: arm64`, source revision, `contains_title_data: false`, the build `validation` and every file's SHA-256. |

It never contains anything derived from a title: no recompiled eboot or prx, CFG or union files,
ledgers, traces or Ghidra projects, and the repository has no title data to begin with. The
package build enforces this: it refuses any file outside the declared install tree (`bin`,
`compiler`, `python`, `lib`, `runtime`, `share`), anything shaped like title data, any file the
assembler did not declare, and a zip whose contents differ from the package directory. Promotion
re-runs the same check on the downloaded package.

## The user's Mac recompiles titles locally

A release ships the recompiler, not recompiled titles. On the first native launch of a title,
MacPS runs the release's `bin/native_aot` against the user's own installed copy and caches the
output on that Mac (the same model as the Rosetta relink cache). The cache stays local, next to the
other per-title work (a private local work directory). Native releases are picked in MacPS next to the
Rosetta ones; MacPS refuses to install one kind as the other.

## Channels

Feeds are `native-alpha.json`, `native-beta.json` and `native-stable.json` (+ `.sig`) on the
rolling `channels` release, product `native`. Build releases are immutable pre-releases tagged
`native-<YYYY.MM.DD>.<n>` (n counts per UTC day, independent of `engine-` tags), titled
"Native engine <version> (alpha)". Signing reuses the engine release key and public key
(docs/RELEASES.md); the signed `release.json` carries `product: native`, `kind: native` and
`arch: arm64`. Nothing here is a required check, and none of it runs on `main`.

| Channel | Gate |
|---|---|
| `native-alpha` | Every push to `native/arm64` (workflow "Native engine release (alpha)"): arm64 build, the AOT ctest suite (`native_aot_*`, `native_graphics_*`, including the hello_notify SDK sample), `native_gpu_cube_check` (frames pixel-identical to the reference), the install-tree guard. |
| `native-beta` | Workflow "Native engine release (promote)", `to: beta`: at least 24 h on alpha (`skip_beta_age` overrides), no open `native-regression` issue, the downloaded package verifies and its manifest shows the alpha gates held with the host not under Rosetta (`sysctl.proc_translated=0`), and the packaged compiler starts. |
| `native-stable` | Same workflow, `to: stable`: the build currently on native-beta, the `confirm_stable` input set (the maintainer's approval that a title reaches the 3D map with captures identical to Rosetta), triggered by the maintainer. |

Pull requests into `native/arm64` only run the release tooling unit tests. A `workflow_dispatch`
of the alpha workflow with `dry_run` builds, tests, packages and signs but publishes nothing.

## Local use

```bash
python3 tools/release/native_alpha.py --source <clean checkout, submodules initialised> \
    --commit "$(git rev-parse HEAD)" --dry-run
python3 -m unittest discover -s tools/tests -p 'test_*release*.py'
python3 tools/release/verify.py https://github.com/RINNECODER/AnyPS5/releases/download/channels/native-alpha.json
```
