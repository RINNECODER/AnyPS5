# Vendored AOT compiler

Source: `boykopovar/AnyPS5` pull request branches, fetched read-only.

| | |
|---|---|
| Base commit | `d4354e41c2cc2b046a117e2773d6a8ae318a71e` |
| PR | [#1835](https://github.com/boykopovar/AnyPS5/pull/1835) (AOT x86-64 → ARM64 compiler), [#3891](https://github.com/boykopovar/AnyPS5/pull/3891) (native ARM64 graphics) |
| Author | Daniele Giovane |
| Fetched | `git fetch --no-tags https://github.com/boykopovar/AnyPS5.git pull/3891/head:refs/remotes/aot/3891 pull/1835/head:refs/remotes/aot/1835` |
| Vendored with | `git checkout aot/3891 -- core/aot core/libs/native core/libs/prx/libc/include/NativeAotImage.hpp` |

Vendored paths:

- `core/aot/` — the offline compiler (Python): Capstone decode, per-instruction LLVM IR
  lowering, `clang` → ARM64 object, contracts, tests, `examples/hello_notify.py`,
  `examples/gpu_cube/`.
- `core/libs/native/` — the native runtime consumer (`NativeAotImage`, `NativeAotPointer`,
  `NativeAotReadonly`, `NativeAotTime`, `NativeAotMain`, the gpu_cube launchers/reference).
- `core/libs/prx/libc/include/NativeAotImage.hpp` — the runtime registration header the
  above includes; it is part of the AOT runtime and is absent from this fork's tree.

Not vendored: the graphics runtime (`core/libs/native/CMakeLists.txt` and the
`ANYPS5_NATIVE_GRAPHICS` build path in upstream's top-level `CMakeLists.txt`), which
compiles the whole `libc`/`libkernel`/`libSceAgc*`/`libSceVideoOut` closure and needs
the full third-party submodule set. This fork has a Metal backend instead of that path.

## Local deviations from the upstream ref

1. **`core/aot/examples/gpu_cube/contracts.json` requalified (4 provider hashes).**
   The contract binds provider source excerpts to whole-file SHA-256 values. Three
   providers differ from the upstream branch only by *unrelated* additions:
   - `core/libs/prx/libc/src/Process.cpp` — adds `LibcBeginHostExit_nid_postfix`
   - `core/libs/prx/libc/src/RuntimeSupport.cpp` — drops `strtoimax`/`strtoumax`, adds
     `__divti3`/`__umodti3`
   - `core/libs/prx/libkernel/System/src/Notification.cpp` — adds a `File`-based stderr
     path in `sceKernelDebugOutText`
   The contracted excerpts (`atexit`/`exit`, `__cxa_atexit`,
   `sceKernelSendNotificationRequest`) are unchanged and still present. Only the
   whole-file hashes were updated. **This is a local requalification and must be reviewed
   before any acceptance claim**; the upstream-qualified hashes are the aot/3891 blobs.
2. **Three AOT submodules cloned by hand** at the aot/3891 pins, because the sandbox
   cannot write `.git/config` and therefore `git submodule update --init` fails with
   `could not write config file .../.git/config: Operation not permitted`:
   - `3rdparty/capstone` `accf4df62f1fba6f92cae692985d27063552601c`
   - `3rdparty/pyelftools` `d4ea35fc3c34eb0e4d3f8cfafb31552f5b5c37d6`
   - `3rdparty/ps5link-sdk` `ea771e535378740b6a058b8e5419eb8a0e0e0ec8`
   `.gitmodules` gained the matching entries; the other third-party submodules remain
   empty and are untouched.
