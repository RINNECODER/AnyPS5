"""Prepare a fresh diagnostic package; execution and build leases belong to the caller.

The schema and fixed CRT payload follow MacPS EnginePackage.accept. No package is
accepted, installed, selected, or described as runtime-ready by this adapter.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile


NATIVE_TESTS = (
    "anyps5_cpu_tests", "anyps5_cpu_context_tests", "anyps5_sce_loader_tests",
    "anyps5_sce_modules_tests", "anyps5_sce_tls_tests", "anyps5_sce_main_lifecycle_tests",
    "anyps5_guest_memory_tests", "anyps5_sce_memory_import_tests",
    "anyps5_sce_videoout_import_tests", "anyps5_sce_native_videoout_tests",
    "anyps5_cpu_metal_test", "anyps5_cpu_memory_metal_test",
    "anyps5_metal_agc_host_exports", "anyps5_guest_thread_tests",
)
FIXTURES = (
    "cpu-homebrew.elf", "sce-homebrew.elf", "sce-module-main.elf", "SceModuleGuest.prx",
    "sce-tls.bin", "cpu-metal-homebrew.elf", "cpu-memory-metal-homebrew.elf",
    "platform-service-main.elf", "PlatformServiceGuest.prx", "kernel-mmu.bin",
    "kernel-pagefault.bin", "ThreadGuest.prx", "thread-main.elf",
)
MANDATORY_FILES = (
    "bin/anyps5_cpu_run", "bin/anyps5_system_probe", "bin/anyps5_sce_modules_tests",
    "fixtures/cpu-homebrew.elf", "fixtures/sce-module-main.elf", "fixtures/SceModuleGuest.prx",
    "fixtures/sce-crt/crt-receipt.txt", "fixtures/sce-crt/sce-crt-main.elf",
    "fixtures/sce-crt/raw/SceCrtGuest.prx", "fixtures/sce-crt/plain-self/SceCrtGuest.prx",
    "bin/anyps5_guest_thread_tests", "fixtures/thread-main.elf", "fixtures/ThreadGuest.prx",
)

# PR85's compiled, bounded production contract. Route claims mean this qualified
# provider subset is assembled by Main; they do not certify a title's imports.
NATIVE_RUNNER_CONTRACT = {
    "enabled": True,
    "owned_memory": "live staged CPU/Metal publication",
    "provider_selection": "actual parsed consumer SHA-256, size, scope and ELF symbol",
    "utility_metallib": "../fixtures/AnyPS5Utilities.metallib relative to engine",
    "wall_limit_ms": 30000,
    "idle_limit_ms": 5000,
    "constraints": "bounded diagnostic profile; qualified provider subset only; high CPU owned stack/TLS are GPU read-only under written-page ABI; no WebAPI2 provider; no retail gameplay evidence",
}
PLATFORM_CASES = (
    ("kernel", "kernel-guest.bin"), ("content", "content-guest.bin"),
    ("network", "network-guest.bin"), ("np", "np-guest.bin"),
    ("audio", "audio-guest.bin"), ("rtc", "rtc-guest.bin"),
    ("sce", "sce-platform.elf"), ("kernel_mutex_threads", "kernel-mutex-threads.elf"),
    ("kernel_priority", "kernel-priority.elf"), ("kernel_round_robin", "kernel-round-robin.elf"),
    ("kernel_condition", "kernel-condition.elf"), ("kernel_static_mutex", "kernel-static-mutex.elf"),
    ("kernel_events", "kernel-events.elf"),
)
VIDEOOUT_CALLERS = tuple("videoout-" + name + ".bin" for name in
                        ("open", "status", "close", "attribute", "register", "rate", "unregister"))
FLIP_CALLERS = tuple("target-flip-" + name + ".bin" for name in ("emit", "wait", "sequence", "submit"))
AGC_CALLERS = tuple("target-agc-" + name + ".bin" for name in
                   ("write", "submit", "shader", "dispatch", "acb-write"))


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def system_library(value):
    """Only canonical Apple system paths may remain external to the package."""
    return value.startswith(("/usr/lib/", "/System/Library/")) and os.path.normpath(value) == value


def prepare_package(source, build, gpu_source, destination, revisions, run, profile="legacy"):
    """Copy and relocate an exact clean source build into a new package directory.

    ``run(name, argv, cwd=None, env=None, timeout=...)`` returns stdout and raises
    on nonzero exit. The caller must prove isolated build identity and complete
    its required regressions before calling; this function records copied-byte
    identities, never infers a passing build from artifact existence.

    Revision values require commit/tree/clean_observed; canonical dependency keys
    are accepted and normalized to schema 2's TCG/Unicorn keys. The returned
    relocation_commands are relative argv lists for caller-owned fixture runs.
    """
    require(profile in ("legacy", "native"), "Unknown diagnostic package profile")
    native = profile == "native"
    source, build, gpu_source = (Path(p).resolve() for p in (source, build, gpu_source))
    destination = Path(destination).absolute()
    require(not destination.exists() and not destination.is_symlink(), "Package destination already exists")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination = destination.parent.resolve() / destination.name
    require(not destination.is_relative_to(source) and not destination.is_relative_to(gpu_source),
            "Package output must be outside source repositories")
    require(not destination.is_relative_to(build), "Package output must be outside the build directory")
    counter = 0

    def command(label, argv, cwd=None):
        nonlocal counter
        counter += 1
        return run(f"package-{counter:03d}-{label}", [str(item) for item in argv], cwd=cwd, timeout=120).strip()

    def git(path, *args):
        return command("git", ["git", *args], cwd=path)

    canonical = {}
    repositories = {"AnyPS5": source, "GPU": gpu_source,
                    "TCG": source / "3rdparty/anyps5-tcg", "Unicorn": source / "3rdparty/unicorn"}

    def check_revisions():
        for key, repository in repositories.items():
            alias = {"TCG": "3rdparty/anyps5-tcg", "Unicorn": "3rdparty/unicorn"}.get(key, key)
            expected = revisions.get(key, revisions.get(alias))
            require(isinstance(expected, dict), "Missing clean source identity: " + key)
            require(expected.get("clean_observed") is True,
                    "No clean source observation supplied for " + key)
            commit, tree = expected.get("commit", ""), expected.get("tree", "")
            require(bool(re.fullmatch(r"[0-9a-f]{40}", commit)) and bool(re.fullmatch(r"[0-9a-f]{40}", tree)),
                    "Full commit and tree identities required for " + key)
            require(Path(git(repository, "rev-parse", "--show-toplevel")).resolve() == repository.resolve(),
                    "Incorrect source repository for " + key)
            require(git(repository, "rev-parse", "HEAD") == commit and
                    git(repository, "rev-parse", "HEAD^{tree}") == tree, "Source revision changed for " + key)
            require(not git(repository, "status", "--porcelain", "--untracked-files=all"),
                    "Source must be clean for " + key)
            canonical[key] = {"commit": commit, "tree": tree, "clean_observed": True,
                              "repository": str(repository.resolve())}
        for key, relative in (("TCG", "3rdparty/anyps5-tcg"), ("Unicorn", "3rdparty/unicorn")):
            entry = git(source, "ls-tree", "HEAD", "--", relative).split()
            require(len(entry) >= 3 and entry[0] == "160000" and entry[2] == canonical[key]["commit"],
                    "Dependency does not match source gitlink: " + relative)

    check_revisions()
    cache_path = build / "CMakeCache.txt"
    require(cache_path.is_file(), "Missing isolated CMake build cache")
    cache_bytes_sha = digest(cache_path)
    cache = {}
    for line in cache_path.read_text().splitlines():
        match = re.match(r"([^/#:][^:]*):[^=]+=(.*)$", line)
        if match:
            cache[match[1]] = match[2]
    wrapper_identity = None
    if native:
        for key in ("BUILD_TESTING", "ANYPS5_CPU_RUNTIME_ONLY", "ANYPS5_BUILD_METAL_SHADER_BRIDGE_TESTS"):
            require(cache.get(key, "").upper() in ("ON", "TRUE", "YES", "1"),
                    "Native diagnostic cache requires " + key + " enabled")
        wrapper = build.parent / "native-controls-source"
        template = Path(__file__).resolve().with_name("native_diagnostic_controls.cmake")
        wrapper_cmake = wrapper / "CMakeLists.txt"
        require(wrapper.is_dir() and not wrapper.is_symlink() and wrapper_cmake.is_file()
                and not wrapper_cmake.is_symlink() and template.is_file(),
                "Missing native diagnostic wrapper source")
        template_sha = digest(template)
        require(digest(wrapper_cmake) == template_sha, "Native diagnostic wrapper content mismatch")
        require(Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() == wrapper.resolve(),
                "CMake build uses a different native wrapper directory")
        require(Path(cache.get("ANYPS5_DIAGNOSTIC_SOURCE", "")).resolve() == source and
                Path(cache.get("ANYPS5_DIAGNOSTIC_CPU_SOURCE_DIR", "")).resolve() == source / "core/cpu" and
                Path(cache.get("ANYPS5_DIAGNOSTIC_CPU_BINARY_DIR", "")).resolve() == build / "source/core/cpu",
                "Native diagnostic wrapper source/build cache mismatch")
        wrapper_identity = {"directory": str(wrapper.resolve()), "cmake_source_sha256": template_sha,
                            "trusted_template": str(template)}
    else:
        require(Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() == source,
                "CMake build uses a different source directory")
    require(Path(cache.get("ANYPS5_CPU_METAL_SOURCE_DIR", "")).resolve() == gpu_source,
            "CMake build uses a different GPU source directory")
    require(cache.get("ANYPS5_CPU_BACKEND") == "TCG", "Package requires the modern TCG backend")
    require(cache.get("CMAKE_OSX_ARCHITECTURES") == "arm64", "Build must explicitly target ARM64")
    runner_flag = cache.get("ANYPS5_CPU_NATIVE_MODULE_RUNNER", "OFF").upper()
    require(runner_flag in (("ON", "TRUE", "YES", "1") if native else ("OFF", "FALSE", "NO", "0", "")),
            "CMake native module runner profile mismatch")

    def capabilities(path, working_directory):
        # Keep ambient dynamic-loader overrides out even when an independent
        # caller supplies a run implementation other than the workflow Runner.
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith(("LD_", "DYLD_"))}
        nonlocal counter
        counter += 1
        raw = run(f"package-{counter:03d}-capabilities", [str(path), "--capabilities-json"],
                  cwd=working_directory, env=environment, timeout=120)
        try:
            def unique_object(pairs):
                result = {}
                for key, value in pairs:
                    require(key not in result, "Duplicate compiled capability key: " + key)
                    result[key] = value
                return result
            value = json.loads(raw, object_pairs_hook=unique_object)
        except (ValueError, TypeError) as error:
            raise RuntimeError("Malformed compiled native capabilities") from error
        require(isinstance(value, dict), "Malformed compiled native capabilities")
        expected = {"schema_version": 1, "host_architecture": "arm64", "guest_architecture": "x86_64",
                    "backend": "Modern QEMU TCG x86-64 dynamic translation", "cpu_profile": "Haswell",
                    "ps5_game_runtime_ready": False, "native_module_runner": NATIVE_RUNNER_CONTRACT,
                    "supported_formats": ["static_elf64_x86_64", "sce_elf64_x86_64"],
                    "supported_containers": ["plain_self"],
                    "supported_instruction_families": ["AVX", "AVX2", "F16C", "FMA"],
                    "runtime_abis": ["linux_sysv", "sce_sysv"], "sce_module_argument": "--sce-module",
                    "resource_root_argument": "--resource-root"}
        require(all(key in value and value[key] == wanted and type(value[key]) is type(wanted)
                    for key, wanted in expected.items()), "Compiled native runner capability contract mismatch")
        # Python equates bools with ints, so independently constrain the bound
        # fields and enabled bit before trusting a JSON contract comparison.
        contract = value["native_module_runner"]
        require(contract.get("enabled") is True and
                type(contract.get("wall_limit_ms")) is int and type(contract.get("idle_limit_ms")) is int,
                "Compiled native runner capability contract mismatch")
        return value, hashlib.sha256(raw.encode("utf-8")).hexdigest()

    def dependencies(path):
        return re.findall(r"^\s*(.+?) \(compatibility version ",
                          command("dependencies", ["otool", "-L", path]), re.M)

    def rpaths(path):
        return re.findall(r"cmd LC_RPATH\n\s+cmdsize \d+\n\s+path (.+?) \(offset",
                          command("rpaths", ["otool", "-l", path]))

    def library_id(path):
        rows = command("install-id", ["otool", "-D", path]).splitlines()
        return rows[1].strip() if len(rows) > 1 else None

    def metadata(path):
        with path.open("rb") as stream:
            header = stream.read(32)
        require(len(header) == 32, "Truncated Mach-O: " + str(path))
        magic, cpu_type, _, file_type = struct.unpack_from("<IIII", header)
        require(magic == 0xfeedfacf and cpu_type == 0x0100000c and file_type in (2, 6),
                "Expected thin native ARM64 executable/dylib: " + str(path))
        require(command("architecture", ["lipo", "-archs", path]) == "arm64",
                "Expected thin ARM64 Mach-O: " + str(path))
        commands = command("load-commands", ["otool", "-l", path])
        if native:
            require("cmd LC_DYLD_ENVIRONMENT" not in commands,
                    "Native artifact embeds a dynamic-loader environment: " + str(path))
        versions = re.findall(r"cmd LC_BUILD_VERSION\n(?:(?!Load command).)*?platform (\d+)\n\s+minos ([0-9.]+)",
                              commands, re.S)
        if versions:
            require(all(platform == "1" for platform, _ in versions), "Non-macOS native artifact")
            floors = [version for _, version in versions]
        else:
            floors = re.findall(r"cmd LC_VERSION_MIN_MACOSX\n\s+cmdsize \d+\n\s+version ([0-9.]+)", commands)
        require(bool(floors), "Mach-O lacks macOS deployment metadata: " + str(path))
        return {"architecture": "arm64", "thin": True, "file_type": file_type,
                "minimum_macos_versions": floors, "dependencies": dependencies(path),
                "rpaths": rpaths(path), "install_id": library_id(path)}

    def expand(value, image, executable):
        if value.startswith("@loader_path/"):
            return image.parent / value[13:]
        if value.startswith("@executable_path/"):
            return executable.parent / value[17:]
        require(value.startswith("/"), "Unsupported native search path: " + value)
        return Path(value)

    cpu = build / ("source/core/cpu" if native else "core/cpu")
    metal = cpu / "metal-source"
    agc = metal / "core/libs/prx/libSceAgc/libanyps5_metal_agc_host_fixture.dylib"
    roots = [cpu / "anyps5_cpu_run", cpu / "anyps5_system_probe"]
    roots += [build / "tests" / name for name in NATIVE_TESTS]
    roots += [agc, cpu / "tcg/libqemu-x86_64-softmmu.dylib"]
    native_agc = metal / "core/shader/recompiler/MetalReplay/cpu-agc"
    platform = cpu / "platform"
    if native:
        roots += [build / "tests/anyps5_native_module_runner_test"]
        roots += [platform / ("anyps5_platform_" + case + "_test") for case, _ in PLATFORM_CASES]
        roots += [platform / ("anyps5_platform_" + case + "_test") for case in
                  ("registration", "kernel_events_owner", "kernel_events_native", "kernel_condition_timeout")]
        roots += [build / "native-videoout" / ("anyps5_cpu_videoout_" + case + "_fixture")
                  for case in ("native", "packed_hdr", "admission")]
        roots += [build / "native-flip" / ("anyps5_cpu_flip_" + case + "_fixture")
                  for case in ("native", "contract")]
        roots += [native_agc / ("anyps5_cpu_agc_" + case + "_fixture")
                  for case in ("native", "capability", "target_events")]
        roots += [build / "tests/anyps5_metal_optional_sgpr_replay",
                  build / "upstream-correctness/anyps5_metal_normalized_load_replay",
                  build / "upstream-correctness/anyps5_metal_scalar_termination_replay"]
    original_runner_sha = digest(roots[0]) if native else None
    if native:
        original_capabilities, original_capabilities_sha = capabilities(roots[0], build)
        require(digest(roots[0]) == original_runner_sha, "Runner changed during build capabilities probe")
    closure, queue = {}, [(path.resolve(), path.resolve(), []) for path in roots]
    while queue:
        image, executable, inherited = queue.pop(0)
        require(image.is_file(), "Missing native package input: " + str(image))
        original_sha = digest(image)
        previous = closure.get(image.name)
        if previous:
            require(previous["sha256"] == original_sha,
                    "Conflicting native basenames: " + image.name)
        before = metadata(image)
        edges = {}
        search = [str(expand(value, image, executable)) for value in before["rpaths"]] + inherited
        for dependency in before["dependencies"]:
            if dependency == before["install_id"] or system_library(dependency):
                continue
            require(not dependency.startswith(("/usr/lib/", "/System/Library/")),
                    "Noncanonical external system dependency: " + dependency)
            candidates = ([Path(value) / dependency[7:] for value in search]
                          if dependency.startswith("@rpath/") else [expand(dependency, image, executable)])
            found = {path.resolve() for path in candidates if path.is_file()}
            require(bool(found), "Unresolved native dependency: " + dependency)
            require(len({digest(path) for path in found}) == 1, "Ambiguous native dependency: " + dependency)
            resolved = sorted(found)[0]
            edges[dependency] = resolved
        if previous:
            require(set(previous["edges"]) == set(edges) and
                    all(previous["edges"][key].name == value.name and
                        digest(previous["edges"][key]) == digest(value) for key, value in edges.items()),
                    "Native dependency resolves differently between root load contexts: " + image.name)
            continue
        queue.extend((resolved, executable, search) for resolved in edges.values())
        closure[image.name] = {"path": image, "sha256": original_sha, "metadata": before, "edges": edges}

    stage = Path(tempfile.mkdtemp(prefix=".diagnostic-package-", dir=destination.parent))
    inputs, binaries = {}, {}

    def copy(origin, relative, executable=False):
        origin = Path(origin)
        require(origin.is_file(), "Missing package input: " + str(origin))
        target = stage / relative
        require(not target.exists(), "Duplicate package target: " + relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        before = digest(origin)
        shutil.copyfile(origin, target)
        target.chmod(0o755 if executable else 0o644)
        require(digest(target) == before and digest(origin) == before, "Input changed while copying: " + relative)
        inputs[relative] = {"original_path": str(origin.resolve()), "original_sha256": before,
                            "original_size": origin.stat().st_size}
        return target

    try:
        for name, record in closure.items():
            is_library = record["metadata"]["file_type"] == 6
            relative = ("lib/" if is_library else "bin/") + name
            require(digest(record["path"]) == record["sha256"], "Native input changed during closure scan")
            target = copy(record["path"], relative, executable=True)
            argv = ["install_name_tool"]
            if is_library:
                argv += ["-id", "@rpath/" + name]
            for dependency, resolved in record["edges"].items():
                argv += ["-change", dependency, ("@loader_path/" if is_library else "@loader_path/../lib/") + resolved.name]
            for old in record["metadata"]["rpaths"]:
                argv += ["-delete_rpath", old]
            if not is_library:
                argv += ["-add_rpath", "@executable_path/../lib"]
            if len(argv) > 1:
                command("relocate", argv + [target])
            command("sign", ["codesign", "--force", "--sign", "-", target])
            command("verify-signature", ["codesign", "--verify", "--strict", target])
            after = metadata(target)
            require(all(system_library(value) or value.startswith("@loader_path/") or value == after["install_id"]
                        for value in after["dependencies"]), "External dependency remains after relocation")
            require(after["rpaths"] == ([] if is_library else ["@executable_path/../lib"]),
                    "Unexpected relocated search paths")
            binaries[relative] = {"original_load_commands": record["metadata"], "packaged_load_commands": after,
                                  "signature": "ad-hoc; codesign --verify --strict returned zero"}
        for relative, record in binaries.items():
            image = stage / relative
            for dependency in record["packaged_load_commands"]["dependencies"]:
                if system_library(dependency) or dependency == record["packaged_load_commands"]["install_id"]:
                    continue
                relocated = expand(dependency, image, image).resolve()
                require(relocated.is_relative_to(stage.resolve()) and relocated.is_file(),
                        "Relocated native dependency escapes or is absent: " + dependency)
        for name in FIXTURES:
            copy(cpu / name, "fixtures/" + name)
        for folder in ("sce-crt", "sce-main-lifecycle"):
            require((cpu / folder).is_dir(), "Missing compiled fixture directory: " + folder)
            for path in sorted((cpu / folder).rglob("*")):
                require(not path.is_symlink(), "Compiled fixture symlinks are unsupported")
                if path.is_file():
                    require(path.suffix in (".elf", ".prx", ".json", ".txt"), "Unexpected compiled fixture payload")
                    copy(path, "fixtures/" + folder + "/" + str(path.relative_to(cpu / folder)))
        copy(cpu / "metal-source/core/libs/prx/libSceAgcDriver/Graphics/Metal/shaders/AnyPS5Utilities.metallib",
             "fixtures/AnyPS5Utilities.metallib")
        if native:
            for _, name in PLATFORM_CASES:
                copy(platform / name, "fixtures/platform/" + name)
            for name in ("thread-main.elf", "ThreadGuest.prx", "kernel-condition-timeout-posix.elf",
                         "kernel-condition-timeout-relative.elf"):
                copy(platform / name, "fixtures/platform/" + name)
            for folder, origin, names in (("videoout", build / "native-videoout", VIDEOOUT_CALLERS),
                                           ("flip", build / "native-flip", FLIP_CALLERS),
                                           ("agc", native_agc, AGC_CALLERS)):
                for name in names:
                    copy(origin / name, "fixtures/" + folder + "/" + name)
            copy(native_agc / "target-agc-events.elf", "fixtures/agc/target-agc-events.elf")
            # A relocated --capabilities-json probe uses the same real process
            # boundary as Main, without executing or mounting any title bytes.
            before_probe = {str(path.relative_to(stage)): digest(path)
                            for path in stage.rglob("*") if path.is_file()}
            packaged_capabilities, packaged_capabilities_sha = capabilities(stage / "bin/anyps5_cpu_run", stage)
            require(packaged_capabilities == original_capabilities,
                    "Relocated capabilities differ from compiled build capabilities")
            require(before_probe == {str(path.relative_to(stage)): digest(path)
                                     for path in stage.rglob("*") if path.is_file()},
                    "Package payload changed during relocated capabilities probe")
            require(digest(roots[0]) == original_runner_sha, "Runner changed after build capabilities probe")
        files = {str(path.relative_to(stage)): {"sha256": digest(path), "size": path.stat().st_size}
                 for path in sorted(stage.rglob("*")) if path.is_file()}
        require(0 < len(files) <= 128 and all(record["size"] <= 268435456 for record in files.values()),
                "Package exceeds strict acceptance file/size limits")
        require(all(name in files for name in MANDATORY_FILES), "Missing strict acceptance fixture role")
        manifest = {"schema_version": 2, "backend": "TCG; native ARM64 host with in-process x86-64 translation",
                    "status": "PREPARED; strict acceptance and relocated fixture execution NOT_RUN by adapter",
                    "source_revisions": canonical,
                    "build_identity": {"directory": str(build), "cmake_cache_sha256": cache_bytes_sha,
                                       "artifact_identity_scope": "Observed copied bytes; caller owns clean build provenance",
                                       "profile": profile},
                    "capability_limits": {"ps5_game_runtime_ready": False, "retail_title_execution_proven": False,
                                          "actual_title_execution": False,
                                          "main_cli_routes_metal_videoout": native, "main_cli_routes_native_agc": native,
                                          "metal_evidence_scope": ("Exact compiled bounded native module provider subset; title execution NOT_RUN"
                                                                   if native else "Native compiled fixtures only")},
                    "binary_load_commands": binaries, "file_inputs": inputs, "files": files}
        if native:
            manifest["build_identity"]["native_wrapper"] = wrapper_identity
            manifest["compiled_capabilities"] = packaged_capabilities
            manifest["capabilities_validation"] = {
                "build_runner_original_sha256": original_runner_sha,
                "packaged_runner_sha256": files["bin/anyps5_cpu_run"]["sha256"],
                "build_capabilities_stdout_sha256": original_capabilities_sha,
                "packaged_capabilities_stdout_sha256": packaged_capabilities_sha,
                "contract": "native_module_runner bounded provider subset matches exactly; original and relocated JSON match",
                "dynamic_loader_environment": "LD_* and DYLD_* removed",
            }
        encoded = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        require(len(encoded.encode()) <= 1048576, "Manifest exceeds strict acceptance size limit")
        (stage / "manifest.json").write_text(encoded)
        (stage / "SHA256SUMS").write_text("".join(digest(path) + "  " + str(path.relative_to(stage)) + "\n"
                                                   for path in sorted(stage.rglob("*")) if path.is_file()))
        for record in inputs.values():
            require(digest(record["original_path"]) == record["original_sha256"], "Input changed during packaging")
        require(digest(cache_path) == cache_bytes_sha, "CMake cache changed during packaging")
        if native:
            require(digest(wrapper_cmake) == template_sha and digest(template) == template_sha,
                    "Native diagnostic wrapper changed during packaging")
        check_revisions()
        require(not destination.exists(), "Package destination appeared during preparation")
        stage.rename(destination)
    except BaseException:
        shutil.rmtree(stage)
        raise

    commands = [["bin/anyps5_cpu_run", "--capabilities-json"],
                ["bin/anyps5_cpu_run", "fixtures/cpu-homebrew.elf"],
                ["bin/anyps5_sce_modules_tests", "fixtures/sce-module-main.elf", "fixtures/SceModuleGuest.prx",
                 "fixtures/sce-crt/crt-receipt.txt"],
                ["bin/anyps5_guest_thread_tests", "fixtures/thread-main.elf", "fixtures/ThreadGuest.prx"],
                ["bin/anyps5_cpu_metal_test", "fixtures/cpu-metal-homebrew.elf", "fixtures/AnyPS5Utilities.metallib"],
                ["bin/anyps5_cpu_memory_metal_test", "fixtures/cpu-memory-metal-homebrew.elf", "fixtures/AnyPS5Utilities.metallib"],
                ["bin/anyps5_sce_native_videoout_tests", "fixtures/AnyPS5Utilities.metallib"],
                ["bin/anyps5_metal_agc_host_exports", "lib/libanyps5_metal_agc_host_fixture.dylib"]]
    if native:
        utility = "fixtures/AnyPS5Utilities.metallib"
        commands += [["bin/anyps5_native_module_runner_test", "fixtures/sce-module-main.elf",
                      "fixtures/SceModuleGuest.prx", utility, case]
                     for case in ("lifecycle", "close", "initializer-failure", "title-agnostic")]
        commands += [["bin/anyps5_cpu_run", "--diagnostics-json", "--sce-module", "fixtures/SceModuleGuest.prx",
                      "fixtures/sce-module-main.elf", "17", "5", "7", "58", "3366582378"]]
        commands += [["bin/anyps5_platform_" + case + "_test", "fixtures/platform/" + name]
                     for case, name in PLATFORM_CASES]
        commands += [["bin/anyps5_platform_registration_test"],
                     ["bin/anyps5_platform_kernel_events_owner_test", "fixtures/platform/thread-main.elf",
                      "fixtures/platform/ThreadGuest.prx"],
                     ["bin/anyps5_platform_kernel_events_owner_test", "fixtures/thread-main.elf", "fixtures/ThreadGuest.prx"],
                     ["bin/anyps5_platform_kernel_events_native_test", "fixtures/platform/kernel-events.elf", utility],
                     ["bin/anyps5_platform_kernel_condition_timeout_test", "fixtures/platform/kernel-condition-timeout-posix.elf",
                      "fixtures/platform/kernel-condition-timeout-relative.elf"]]
        video = ["fixtures/videoout/" + name for name in VIDEOOUT_CALLERS]
        commands += [["bin/anyps5_cpu_videoout_" + case + "_fixture", utility, *video]
                     for case in ("native", "packed_hdr")]
        commands += [["bin/anyps5_cpu_videoout_admission_fixture", *video]]
        flip = ["fixtures/flip/" + name for name in FLIP_CALLERS]
        commands += [["bin/anyps5_cpu_flip_contract_fixture", *flip],
                     ["bin/anyps5_cpu_flip_native_fixture", utility, *flip],
                     ["bin/anyps5_cpu_flip_native_fixture", utility, *flip, "shutdown"],
                     ["bin/anyps5_cpu_agc_capability_fixture"],
                     ["bin/anyps5_cpu_agc_native_fixture", utility, *["fixtures/agc/" + name for name in AGC_CALLERS]],
                     ["bin/anyps5_cpu_agc_target_events_fixture", "fixtures/agc/target-agc-events.elf", utility]]
        commands += [["bin/anyps5_metal_optional_sgpr_replay", mode, utility]
                     for mode in ("compute", "draw-vertex", "draw-fragment", "required")]
        commands += [["bin/anyps5_metal_normalized_load_replay"],
                     ["bin/anyps5_metal_scalar_termination_replay", "native"],
                     ["bin/anyps5_metal_scalar_termination_replay", "decode"]]
    return {"package": str(destination), "manifest_sha256": digest(destination / "manifest.json"),
            "checksums_sha256": digest(destination / "SHA256SUMS"), "artifacts": files,
            "input_artifacts": inputs, "relocation_commands": commands,
            "acceptance_status": "NOT_RUN", "profile": profile, "game_runtime_ready": False,
            "actual_title_execution": False}
