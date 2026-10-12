# Rosetta direct execution on Apple silicon (feasibility spike)

Status: spike, 2026-10-11. Prototype tooling is in [`tools/rosetta-spike`](../../tools/rosetta-spike) and [`tools/unself.py`](../../tools/unself.py). Nothing here changes the shipped runtime.

## Summary

Running guest code directly in an x86-64 process under Rosetta 2 is feasible on this fork, and most of the CPU side already exists upstream.

- Upstream's relinker already converts a title into an x86-64 Mach-O for Rosetta (`relinker --macos --to-rosetta`). The fork has this through the upstream sync. What upstream does not have is the macOS runtime that the output binds to. Those hooks are not implemented anywhere, and the system libraries do not build for macOS.
- With a ~150-line stub runtime, the reference retail title, relinked unchanged, ran natively under Rosetta. Both bundled guest modules' initialisers ran, the title's TLS was registered, the guest entry point was reached, and the guest made 633 import calls across 23 distinct NIDs. All of that took **56 ms of wall time from exec, warm**. It then stopped on a NULL dereference, because every import is a stub that returns 0. A cold first run takes 6.4 s, because Rosetta translates the 160 MB image ahead of time once and caches the result.
- On a guest-like integer, memory and scalar-FP loop, Rosetta ran **~4.7 G x86 instructions/s**. Block-chained QEMU TCG (Unicorn 2.1.4) ran ~0.75 G/s. The fork's single-stepping TCG path runs at ~16 MIPS (#125, #140, #141, #210). That puts Rosetta about **6x faster than the best TCG case and ~300x faster than today's path**. Native arm64 code was only 1.8x faster than Rosetta.
- The fork's Metal backend builds for x86_64 without changes, and its GPU tests pass inside a Rosetta process. Metal is therefore the realistic GPU backend. MoltenVK is not needed.
- The remaining work is porting the system libraries (`core/libs/prx`) to macOS x86-64 and writing the runtime hooks. A compile probe of the 41 CPU-side libraries the title links finds **27 failing translation units out of ~170**, all in `libc`, `libkernel`, `libSceFiber` and `libSceAudioOut`, with a short, known list of Linux-only APIs. The other libraries compile as they are.

Platform risk: Apple announced at WWDC 2025 that full Rosetta 2 stays available through macOS 27 (this machine runs it), and that later releases keep only a subset aimed at older games. This path depends on Rosetta staying available, so the TCG path has to remain as the fallback.

## 1. How upstream executes guest code

Upstream never interprets guest code. It relinks the guest ELF ahead of time into a native executable for an x86-64 host (see [ARCHITECTURE.md](../dev/ARCHITECTURE.md)):

- **Loader and relinker.** `core/relinker` reads the guest ELF and its bundled `sce_module/*` modules. It lowers AMD-only instructions with `--to-intel`, and with `--to-rosetta` it also turns RDSEED into RDRAND, CLWB into a NOP and RDPID into "processor 0". It checks for raw syscalls and writes an ELF (Linux), PE (Windows) or Mach-O (`--macos`) image.
- **Imports.** Each import is bound by NID to a host shared library, `libs/<name>.prx`, built from `core/libs/prx` with exports renamed to their NIDs by `nid_patcher`. On macOS the output uses classic `LC_DYLD_INFO` bind opcodes with flat lookup (21,270 binds over 1,757 distinct symbols for the reference title), plus one `LC_LOAD_DYLIB @rpath/<lib>.prx` for each of the 45 libraries.
- **Fixed mappings.** The Mach-O executable keeps the guest's segment layout at a fixed base. The reference title loads at `0x140000000` with `__PAGEZERO` extended below it. The output is not PIE (`MH_PIE` is clear), and the kernel and dyld mapped it under Rosetta without complaint.
- **FS-base TLS.** macOS cannot set `%fs`, so every `%fs:` access (about 2,000 sites in the reference title) is rewritten into a jump to a stub. The stub reads a per-image thread pointer from a pthread key (`%gs:[key*8]`, the Darwin TSD fast path). On a thread's first access it calls `Aps5GuestTlsAllocate` with the TLS descriptor in `%rax`. Only displacements inside the static TLS block, `0` (self pointer) and `0x28` (stack guard) are accepted. Dynamic TLS goes through the image's descriptor instead of `__tls_get_addr`.
- **Start-up.** `LC_MAIN` points at an entry stub that builds a FreeBSD-style `argc/argv/envp` frame and calls `Aps5StartGuest(entry, frame)`. Guest modules get a `__mod_init_func` that registers TLS, fetches module-start arguments through `__aps5_get_pending_module_args`, runs the module entry and reports the result.
- **Signals.** On macOS, the write watch (`libc/src/GuestWriteWatch.cpp`) and the register-context unwinder (`libc/src/specifics/x86_64/RegisterContext.cpp`) use SIGSEGV/SIGBUS and the Darwin x86-64 `mcontext`. Their standalone tests are built to run under Rosetta.

**How complete the macOS path is.** Only the relinker side is in the tree. `Aps5StartGuest`, `Aps5GuestTlsRegister` and `Aps5GuestTlsAllocate` have no implementation anywhere. [BUILD.md](../dev/BUILD.md) says the relinker-only mode "does not build those libraries or provide macOS game execution". [TechnicalDebt.md](../dev/TechnicalDebt.md) says "the macOS memory paths are not in the tree".

## 2. Can an x86_64 build run on this Mac, and with which GPU backend?

| Piece | Result under Rosetta (M5 Pro, macOS 27) |
| --- | --- |
| Relinker (built arm64) on the reference title, `--macos --to-rosetta` | OK in 29 s. Produced a 160 MB Mach-O, two guest-module dylibs, and 43 instruction conversions (10 in place, 33 stubs) |
| Kernel and dyld load of the relinked image | OK. The fixed `0x140000000` base, 11 segments and flat binds all resolve |
| Guest module initialisers, TLS register, guest entry, import calls | OK with the spike runtime, as described above |
| CPU-side system libraries for x86_64 macOS | 27 of ~170 translation units fail (section 3.6) |
| Fork Metal backend (`ANYPS5_METAL_UTILITIES_ONLY`) built for x86_64 | Builds unchanged. `metal_utility_tests` passes, including the guest-memory, presentation and sample-counter GPU checks |

**GPU backend: our Metal backend, built for x86_64.** Metal runs in Rosetta processes, and the backend compiles and passes its tests there. That covers the work already done on the fork: AGC submission, the SPIR-V to MSL bridge, presentation and guest-memory mapping. Upstream's Vulkan backend via MoltenVK would mean building Vulkan, glslang and SPIRV-Tools for x86_64 and adding another translation layer. It would also lose the Metal-specific fixes. One simplification applies: the guest pointer and the host pointer are now the same, so the borrowed guest ranges given to `MetalDriver::Configure` become plain host pointers. The TCG-side translation is not needed.

## 3. Rosetta-specific blockers and measurements

Run `tools/rosetta-spike/run_probe.sh <scratch>` to repeat these.

1. **Page size.** An x86_64 process sees `getpagesize() == vm_page_size == 4096`. `mprotect` on a single 4 KiB page inside a 16 KiB host page works. Writes to the neighbouring 4 KiB pages do not fault, and a write to the protected page faults once with the exact address. `MAP_FIXED` at 4 KiB-aligned addresses works. A `shm_open` object mapped at a 4 KiB file offset into a fixed 4 KiB slot is coherent with a second view, which is the direct-memory aliasing model. Native arm64 rejects all three of these (16 KiB granularity). PS5 direct memory uses 16 KiB pages (`PS5_PAGE_SIZE = 0x4000`), so granularity is not a blocker.
2. **Fixed guest address ranges.** Fixed mappings are refused with `EACCES` in **0x10_0000_0000–0x80_0000_0000 (64–512 GiB)**, both natively and under Rosetta. Below that (8–64 GiB) and from 512 GiB up, mappings work, and 64 GiB fixed reservations at 1 TiB and 126 TiB succeed. Upstream's `DirectMemory` already searches with `MAP_FIXED_NOREPLACE` up to `0x7fff00000000`, so it can avoid the hole. Only a title that asks for a fixed address inside 64–512 GiB would fail. The fix is the same as on other hosts: relocate or report.
3. **CPUID.** Rosetta reports a GenuineIntel family-6 CPU with SSE4.2, POPCNT and AES. It reports AVX, AVX2, FMA, BMI1/2, F16C, MOVBE, XSAVE/OSXSAVE and RDRAND as 0, but executes AVX2, FMA, BMI2 and F16C correctly (the `host_x86_avx2` build of the benchmark gives the same result). The reference title contains ~835k VEX-prefix bytes, so it is AVX code compiled for Zen 2. A byte scan finds ≤70 CPUID and ≤5 XGETBV sites (an over-count). Most console code uses AVX unconditionally and runs. Code that checks CPUID would take a slower path or refuse. The fix belongs in the relinker: rewrite CPUID sites into a stub that returns a Zen 2 profile, using the stub mechanism `--to-rosetta` already has. ADCX/ADOX fault under Rosetta. RDTSCP's `ECX` is the constant `0xAAAAAAAB`.
4. **x87 and AVX coverage.** Rosetta's x87 emulation is reported to compute in double precision, not 80-bit. This spike did not measure it. The microbenchmark's SSE and AVX builds give identical checksums across native, Rosetta and TCG, and no title-level x87 problem showed up within the spike's reach.
5. **Signals.** SIGSEGV/SIGBUS deliver the x86 `__ss.__rip` and the exact fault address under Rosetta. A protect-write-fault-unprotect round trip costs **~44 µs**. That is fine for guard pages, but expensive for a write watch that faults on every first write to a GPU-visible page. Batching or Metal-side dirty tracking will matter.
6. **System libraries do not build for macOS.** Configuring `tools/rosetta-spike/libs-probe` with `CMAKE_OSX_ARCHITECTURES=x86_64` and building with `ninja -k 0` gives these failures:
   - `libc` (10 TUs): ELF-only `.type sym, @function` in inline asm (setjmp, longjmp, getcontext) and `feof_unlocked`.
   - `libkernel` (13 TUs):
     - `link.h`/`dl_iterate_phdr`, which needs the dyld image APIs (`libc/src/specifics/macos/GuestImages.cpp` exists).
     - `sys/vfs.h`.
     - `memfd_create`, to be replaced with `shm_open` or `mach_make_memory_entry`.
     - `ppoll` and `POLLRDHUP`.
     - `RUSAGE_THREAD`, `sched_getcpu`, `pthread_getattr_np` and `pthread_getcpuclockid`, to be replaced with `pthread_get_stackaddr_np` and `thread_info`.
     - `st_atim` and friends, which are `st_atimespec` on macOS.
     - Linux `gregs` in `Exception.cpp`, which become `mcontext->__ss`.
     - `DirectMemory.cpp`, whose non-Linux branch is the Windows one.
   - `libSceFiber`: the same `.type` directive.
   - `libSceAudioOut`: needs SDL2.
   - All other probed libraries compile. `libHttpServer.Prospero` has no source directory, and `libSceAjm.native` and the graphics libraries need submodules (not probed).

## 4. Measurements

All runs were on an Apple M5 Pro running macOS 27. The kernel is [`tools/rosetta-spike/kernel.c`](../../tools/rosetta-spike/kernel.c): a hash, a dependent 16 KiB table load and store, and a branchy scalar double update. It is 22.5 x86 instructions per iteration, counted under Unicorn, and has no calls or literal pools, so its bytes run unchanged under TCG.

| Engine | Iterations | Time | Guest instr/s | Checksum |
| --- | --- | --- | --- | --- |
| Native arm64 build (reference) | 200 M | 0.529 s | n/a | `9f938a1e…` |
| Rosetta, x86_64 SSE build | 200 M | 0.949 s | **4.74 G** | `9f938a1e…` |
| Rosetta, x86_64 AVX2/FMA/BMI2 build | 200 M | 0.968 s | 4.65 G | `9f938a1e…` |
| QEMU TCG, block-chained, no hooks (Unicorn 2.1.4) | 20 M | 0.597 s | 0.75 G | `034e31c0…` (matches Rosetta at 20 M) |
| AnyPS5 TCG path today (single-step, #125/#140/#141/#210) | n/a | n/a | ~0.016 G | n/a |

The table shows Rosetta about 6x faster than the best TCG case, about 300x faster than today's single-stepping path, and 1.8x slower than native arm64 code.

Reference title milestones:

| Milestone | Rosetta direct (spike runtime, all imports stubbed) | TCG today (same day, same machine) |
| --- | --- | --- |
| Relink (one-time, cacheable) | 29 s | none |
| Runtime load to both guest module initialisers done | 7.6 ms (a cold image adds a one-time ~6.3 s Rosetta AOT translation before this) | not separately logged |
| Guest entry point reached | 3.6 ms warm / 8.1 ms cold after runtime load | not separately logged |
| First 633 import calls | within 16 ms of runtime load (56 ms wall from exec, warm) | not separately logged |
| First GPU submission / rendering | not reached (imports are stubs) | 1,628 s (smoke run today; 1,530 s earlier), one core at 100%, 13.8 GB peak RSS |

A rough projection: if the 1,628 s TCG boot is CPU-bound at ~16 MIPS, it is ~26 G guest instructions. At the measured 1–5 G/s, that work takes 5–25 s under Rosetta. The real figure will depend on how the libraries perform (file I/O, shader translation, memory mapping), not on guest instruction throughput.

## 5. Phased plan

| Phase | Work | Estimate |
| --- | --- | --- |
| 0 (this spike) | `tools/unself.py` (plaintext SELF to ELF, mirrors `core/cpu/src/Self.cpp`). Relink with `--macos --to-rosetta`. `tools/rosetta-spike/runtime`: start, TLS and module-argument hooks, plus logging NID stubs. Environment probe. Benchmark. Libs compile probe. | done |
| 1: macOS x86-64 system libraries | Fix the 27 failing TUs listed in 3.6. Add a Darwin branch to `DirectMemory`: `shm_open` or Mach memory entries for physical memory, `MAP_FIXED` views, and avoiding the 64–512 GiB hole. Build `libs/*.prx` with `nid_patcher` (it already patches Mach-O). Add a CMake mode for `CMAKE_OSX_ARCHITECTURES=x86_64` without SDL/ffmpeg at first. | 1–1.5 weeks |
| 2: runtime hooks and threading | Move `Aps5StartGuest` and `Aps5GuestTlsRegister/Allocate` into libkernel (spike versions exist). Thread creation must set up guest TLS per image. Load modules dynamically (`sceKernelLoadStartModule`) through `dlopen` of the relinked `*.guest.prx`. Wire exception handling to the macOS mcontext. | 1 week |
| 3: graphics and I/O | Link the fork's Metal backend into the x86_64 `libSceAgcDriver` and `libSceVideoOut` builds, and drop guest-to-host translation. Build the shader recompiler and its submodules for x86_64. Bring the native providers from `core/cpu` (presentation, pad, audio) in as prx-side implementations. | 2–3 weeks |
| 4: CPUID, performance, launcher | Add a relinker CPUID stub that advertises Zen 2 (AVX2, FMA, BMI). Batch the write watch or use Metal dirty tracking. Cache the per-title relink keyed by the eboot digest. Have the launcher pick the path (section 6). Run a smoke comparison against TCG. | 1 week |

The total is roughly 5–7 weeks of focused work to reach first rendering on the direct path. Phase 1 is the critical path. It is also the part most likely to bring upstream changes along, since upstream's own macOS runtime "comes in separate changes".

### Phase 2 status (runtime hooks)

- **Runtime.** [`core/libs/prx/libkernel/Darwin/GuestRuntime.cpp`](../../core/libs/prx/libkernel/Darwin/GuestRuntime.cpp) is built into the x86_64 `libkernel.prx` only. `Aps5StartGuest` builds a FreeBSD `argc/argv/envp/auxv` frame (`argv[0]` is `/app0/eboot.bin`, `AT_PAGESZ` is 16 KiB) and exits with the entry's result. Static TLS is per image and per thread, keyed by each image's pthread key and allocated on a thread's first access, so threads from `scePthreadCreate`, `pthread_create` or host libraries, and modules loaded later, all get every image's TLS. `Aps5GuestTlsGetAddr` serves guest `__tls_get_addr`. Blocks are freed when the thread exits.
- **Module order and loading.** dyld runs each image's `__mod_init_func` after its dependencies, so the bundled guest `libc` starts before the modules and the executable that import it. The relinker links `sce_module/*` as load-time dependencies. Modules under `--module-dir` are relinked but deferred, and `sceKernelLoadStartModule` `dlopen`s them with module arguments. Module ids now match `dlopen` handles on Darwin, because the loader records each handle's Mach-O header.
- **Faults.** SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP are reported from the moment `libkernel` loads. The report gives the fault address, `image+offset`, the guest address (`guest=`), the registers and a frame-pointer backtrace. The handler chains to the libc write watch first. Real faults are not yet delivered to guest exception handlers. `sceKernelRaiseException` keeps using the `__darwin_mcontext64` mapping from #394.
- **The 64–512 GiB hole.** Titles map at the start of the PS5 application area (`0x10_0000_0000`), which macOS refuses. Fixed requests that start in the hole are moved up by 448 GiB, to `[512, 960) GiB`, and the new address is returned through `*addr`. That keeps them below `0xFC_0000_0000`, which the guest libc checks pointers against. A request that starts below the hole and runs into it still fails with `ENOMEM`.
- **Running a title.** `tools/rosetta/run_title.py <title> --resource-root <app0> --build <x86_64 build>` unselfs and relinks the title, caches the result by eboot SHA-256 in `~/Library/Caches/AnyPS5/rosetta/<sha>/`, and lays out `libs/` and `app0/`. It runs the title with a wall limit and prints a summary. Libraries the build excludes get empty stand-ins. Imported NIDs that no library exports get logging stubs that return 0 (`DYLD_INSERT_LIBRARIES`, passed through `arch -e`).
- **Reference title, 2026-10-11.** The guest entry is reached 0.63 s after start-up (1.8 s wall in all). The title then calls the stubbed `sceVideoOut*` and stops on `sce::Json::InitParameter2` (libSceJson2, not implemented). The TCG path takes ~1,630 s to its first draw. Next blockers: the excluded `libSceVideoOut`, `libSceAgc` and `libSceAgcDriver` (phase 3), `libSceAjm.native`, the guest-shipped `libHttpServer.Prospero`, and gaps in the libraries themselves. The first run on a new machine took 513 s before dyld failed, and the cause was not investigated. Later runs take 2–6 s.
- **Reference title with the graphics libraries, 2026-10-11.** The title now runs with real Metal submissions: guest entry 0.58 s after start-up, first flip about 8 s after launch (warm), and it keeps running (200 s checked) at about 1.2 guest flips per second. The captured frames are still near-black (no channel above 8). The fixes on the way were: `___stack_chk_guard` kept as a plain libc export for host code, macOS executable paths, unreadable `PT_DYNAMIC`, `scePthreadCancel` on joined threads, VideoOut structures on thread stacks, borrowed ranges split under `maxBufferLength`, only GPU-mapped memory borrowed, and no no-copy aliases of file-backed (image) pages, whose wiring caused a SIGBUS without an address under Rosetta. `run_title.py` lets unimplemented library functions log and return 0 (`ANYPS5_UNIMPLEMENTED=continue`), and the fault report covers SIGABRT, so uncaught host exceptions show the throw site. Open items: the borrowed direct memory (13.5 GiB, one GPU mapping) is wired whole while the GPU uses it, which leaves little memory on a 24 GiB machine; the slow flip rate and dark frames; and shaders that fail to compile (`memory_order_seq_cst`), on both paths.
- **GPU memory, shader compiles and flip rate, 2026-10-11.** A command buffer wires the host pages of every no-copy alias it uses, so guest memory is now bound per working set: captures alias only 4 MiB host granules that a binding or a GPU fault has asked for (`MetalGuestHostAliases::EnableWorkingSet`). A CPU-side miss sets the work up again, and a shader access outside the set reads zero, drops its store and reports an Unmapped fault, so the work runs again once the bytes are admitted. Over its budget (3 GiB; `ANYPS5_METAL_WORKING_SET_MIB`, 0 binds the whole mapping as before) the set starts empty. The reference title's set stays near 120 MiB, and wired memory peaks 0.6 GB above the idle baseline instead of 16 GB. The `memory_order_seq_cst` errors came from the language version: the relinked eboot declares SDK 11, whose default is MSL 2.3, while the bridge emits MSL 3.2, so converted shaders now compile as at least MSL 3.2. The flip rate was bound by the GPU queue worker, which uploaded and read back the surfaces of every draw and dispatch through the CPU (`replaceRegion`/`getBytes` inside the translated Metal driver, plus staging copies). Textures now detile straight from the snapshot mirror and blit on the GPU, and uploads no longer wait: about 10 guest flips per second instead of 1.4. The frames are still near-black.
- **Tests.** `ctest -L rosetta` in an x86_64 `-DANYPS5_ROSETTA_LIBS=ON` build needs `ld.lld`. It builds small guest fixtures from [`tools/rosetta/tests`](../../tools/rosetta/tests), relinks them and runs them under `arch -x86_64`. They check per-thread TLS in the executable, a startup module and a `dlopen`ed module across four `scePthreadCreate` threads, `module_start` arguments and result, `sceKernelDlsym`, and module-info ids. A second test checks the fault report.

## 6. Coexistence with the TCG path

- **Two engines, one set of libraries.** `anyps5_cpu_run` (arm64, QEMU TCG) stays as it is and remains the fallback. The direct path is a separate x86_64 launch: a relinked `eboot` plus `app0/sce_module/*.guest.prx`, and `libs/*.prx` built for x86_64 macOS. Both paths share `core/libs` sources and the Metal backend. Only the pointer translation and the `core/cpu` loader are TCG-only.
- **Selection at launch.** Use direct execution when Rosetta is installed (`arch -x86_64 /usr/bin/true` succeeds), the relink succeeded, and the title is not on a deny list. Otherwise use TCG. Each title gets a cache directory holding the relink output, keyed by the input digest.
- **Fallback triggers.** Fall back to TCG on a relink refusal (unsupported TLS form, branch into a rewritten TLS instruction, forbidden syscall), on a fixed mapping in the reserved range, or when a future macOS drops general Rosetta support.
- **Shared tests.** The environment probe and the benchmark go into CI on Apple silicon runners as Rosetta canaries.

## Reproduce

```sh
cmake -S . -B build-relinker -G Ninja -DCMAKE_BUILD_TYPE=Release -DANYPS5_RELINKER_ONLY=ON
cmake --build build-relinker --target relinker
python3 tools/unself.py <title>/eboot.bin src/input.elf          # and each sce_module/*.prx into src/sce_module/
build-relinker/core/relinker/relinker --macos --to-rosetta src/input.elf out/eboot
chmod +x out/eboot
python3 tools/rosetta-spike/runtime/build_runtime.py out <work>   # stub libs/ for dyld
(cd out && SPIKE_SECONDS=60 arch -x86_64 ./eboot)                 # logs first import calls, then stops
tools/rosetta-spike/run_bench.sh <scratch> <python-with-unicorn>
tools/rosetta-spike/run_probe.sh <scratch>
```
