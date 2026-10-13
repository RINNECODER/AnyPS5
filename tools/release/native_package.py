#!/usr/bin/env python3
"""Build, test, verify and zip one native ARM64 engine package from a clean checkout.

    python3 tools/release/native_package.py --source <clean checkout with submodules> \
        --commit <40-hex HEAD> --version 2026.10.12.1 \
        --work <new directory without personal paths, e.g. /private/tmp/anyps5-release/x>

The native engine is the offline x86-64 -> ARM64 compiler (core/aot) plus the arm64 runtime
archives a recompiled title links against (fork Metal backend). Steps (docs/design/native-releases.md):

1. clone --source at --commit (and its submodules, from the source's own copies) into --work;
2. configure the arm64 build (-DANYPS5_NATIVE_AOT=ON -DANYPS5_NATIVE_AOT_GRAPHICS=ON
   -DANYPS5_NATIVE_AOT_EXAMPLE=ON), build the AOT tools, runtime and gpu_cube fixture;
3. run the AOT ctest suite (``native_aot_*`` and ``native_graphics_*``, which includes the
   hello_notify SDK sample) and ``native_gpu_cube_check`` (AOT vs reference frames, pixel-identical);
4. assemble the package from a fixed, declared install tree (bin, compiler, python, lib, runtime,
   share) and refuse any file outside it, any title-shaped file, any non-arm64 binary and any
   byte that holds the build user's home path or user name;
5. write manifest.json (``kind: native``, ``arch: arm64``) and SHA256SUMS, write a deterministic
   zip, round-trip it with the release extractor and run the packaged compiler's ``--help``.

Signing and publishing stay in native_alpha.py. The package holds code only: nothing derived from
a title is ever produced here, and the repository has no title data to include.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import sys

import build_package
import releaselib
import verify

NATIVE_SUBMODULES = build_package.SUBMODULES + ("3rdparty/capstone", "3rdparty/pyelftools", "3rdparty/ps5link-sdk")
BACKEND = "Native ARM64 AOT (offline x86-64 to ARM64 compiler, arm64 runtime on the Metal backend)"
CONFIGURE = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON", "-DCMAKE_OSX_ARCHITECTURES=arm64",
             "-DANYPS5_NATIVE_AOT=ON", "-DANYPS5_NATIVE_AOT_GRAPHICS=ON", "-DANYPS5_NATIVE_AOT_EXAMPLE=ON"]
BUILD_TARGETS = ["native_gpu_cube_build", "native_aot", "anyps5_native_runtime", "anyps5_native_graphics", "aot_ps5link",
                 "native_aot_graphics_layout_tests", "native_aot_readonly_tests", "native_aot_image_tests",
                 "native_aot_runtime_tests"]
CTEST_FILTER = "^native_(aot|graphics)"
# Tests the gate cannot pass without (the SDK samples and the Metal graphics checks).
REQUIRED_TESTS = ("native_aot_hello_notify", "native_aot_runtime", "native_graphics_image_comparison",
                  "native_graphics_reference_policy")
MIN_TESTS = 100

# The declared install tree: top-level directories and the file kinds each may hold. Anything else
# (and anything shaped like title data) fails the build.
ALLOWED_ROOTS = {
    "bin": {""},
    "compiler": {".py"},
    "python": {".py"},
    "lib": {".dylib"},
    "runtime": {".a", ".hpp"},
    "share": {".metallib", "", ".txt", ".TXT", ".md"},
}
TOP_LEVEL_FILES = {"manifest.json", "SHA256SUMS"}
# Extensions and names a title (or a recompiled title) would carry. Never part of a native package.
TITLE_SUFFIXES = {".bin", ".elf", ".self", ".sprx", ".prx", ".pkg", ".pfs", ".trp", ".sfo", ".o", ".obj", ".ll", ".ghidra",
                  ".gpr", ".rep", ".img", ".dmg", ".iso", ".dump", ".trace"}
TITLE_NAMES = {"eboot.bin", "param.sfo", "param.json", "sce_sys", "sce_module", "cfg.json", "ledger.json"}


def gate_failure(message):
    raise releaselib.ReleaseError("native package guard: " + message)


def check_install_tree(package, declared):
    """Every file in ``package`` is in ``declared`` (the assembler's own list), sits under an allowed
    root with an allowed suffix, and is not shaped like title data. ``manifest.json`` and
    ``SHA256SUMS`` are the only top-level files. Raises ReleaseError listing the offenders."""
    package = Path(package)
    declared = set(declared)
    problems = []
    for path in sorted(package.rglob("*")):
        relative = path.relative_to(package)
        name = str(relative)
        if path.is_symlink():
            problems.append(name + " is a symbolic link")
            continue
        if path.is_dir():
            if len(relative.parts) == 1 and relative.parts[0] not in ALLOWED_ROOTS:
                problems.append(name + "/ is not a declared install root")
            continue
        if name in TOP_LEVEL_FILES:
            continue
        if len(relative.parts) == 1 or relative.parts[0] not in ALLOWED_ROOTS:
            problems.append(name + " is outside the declared install tree")
            continue
        if name not in declared:
            problems.append(name + " was not declared by the assembler")
        if path.suffix not in ALLOWED_ROOTS[relative.parts[0]]:
            problems.append(name + " has a suffix not allowed under " + relative.parts[0] + "/")
        if path.suffix.lower() in TITLE_SUFFIXES or path.name.lower() in TITLE_NAMES or \
                any(part.lower() in TITLE_NAMES for part in relative.parts):
            problems.append(name + " looks like title data")
    for name in sorted(declared):
        if not (package / name).is_file():
            problems.append(name + " was declared but is missing")
    if problems:
        gate_failure("; ".join(problems[:12]) + (" ... and %d more" % (len(problems) - 12) if len(problems) > 12 else ""))


def check_zip_tree(archive, package):
    """The zip holds exactly the package tree: same files, no extras."""
    import zipfile
    with zipfile.ZipFile(archive) as bundle:
        in_zip = {name for name in bundle.namelist() if not name.endswith("/")}
    on_disk = {str(path.relative_to(package)) for path in Path(package).rglob("*") if path.is_file()}
    if in_zip != on_disk:
        gate_failure("zip contents differ from the package tree: %s" %
                     ", ".join(sorted(in_zip ^ on_disk)[:12]))


def parse_ctest(text):
    """({name: status}, passed_line_count_total) from ``ctest`` output."""
    results = {}
    for match in re.finditer(r"Test\s+#\d+:\s+(\S+)\s+\.+\s*(?:\*\*\*)?(Passed|Failed|Not Run|Timeout|Exception|Skipped)", text):
        results[match.group(1)] = match.group(2)
    return results


def check_ctest(results):
    failed = sorted(name for name, status in results.items() if status != "Passed")
    releaselib.require(not failed, "native tests did not pass: " + ", ".join(failed[:10]))
    releaselib.require(len(results) >= MIN_TESTS, "only %d native tests ran (expected at least %d)" % (len(results), MIN_TESTS))
    missing = [name for name in REQUIRED_TESTS if name not in results]
    releaselib.require(not missing, "required native tests did not run: " + ", ".join(missing))


class Skipped(Exception):
    """The release cannot be validated on this host right now (not a failure); nothing is published."""


# Wall-clock bounds (seconds) so a wedged step fails the build instead of hanging the runner.
STEP_TIMEOUTS = {"configure": 900, "build": 3600, "ctest": 1800, "gpu-cube-check": 300}


def gui_session_unavailable():
    """Reason string when this process has no unlocked console GUI session, else None.

    Same condition the AppKit focus test skips on (exit 77): a self-hosted laptop runner with a locked screen.
    ``ioreg`` lists the console users with ``CGSSessionScreenIsLocked`` / ``kCGSSessionOnConsoleKey``."""
    result = subprocess.run(["ioreg", "-n", "Root", "-d1", "-a"], capture_output=True, timeout=30)
    if result.returncode != 0:
        return None
    root = plistlib.loads(result.stdout)
    root = root[0] if isinstance(root, list) else root  # `ioreg -a` prints one dict, or an array holding it
    users = root.get("IOConsoleUsers", [])
    if root.get("IOConsoleLocked"):
        return "the screen is locked"
    if not users:
        return "there is no console GUI session"
    if any(user.get("CGSSessionScreenIsLocked") for user in users):
        return "the screen is locked"
    if not any(user.get("kCGSSessionOnConsoleKey") for user in users):
        return "this account is not on the console"
    return None


def require_gui_session(step):
    reason = gui_session_unavailable()
    if reason:
        raise Skipped("%s needs a GUI session but %s" % (step, reason))


def host_check():
    releaselib.require(platform.machine() == "arm64", "native packages are built and tested on an arm64 host")
    translated = subprocess.run(["sysctl", "-n", "sysctl.proc_translated"], capture_output=True, text=True)
    value = translated.stdout.strip() if translated.returncode == 0 else "0"
    releaselib.require(value == "0", "the build is running under Rosetta (sysctl.proc_translated=%s)" % value)
    return {"machine": platform.machine(), "proc_translated": int(value)}


def link_closure(build, logs, log):
    """Static archives, frameworks and system libs the gpu_cube link uses, in link order, from ninja."""
    result = subprocess.run(["ninja", "-C", str(build), "-t", "commands", "native_gpu_cube"], capture_output=True, text=True)
    releaselib.require(result.returncode == 0 and result.stdout.strip(), "could not read the gpu_cube link command")
    tokens = result.stdout.strip().splitlines()[-1].split()
    archives, frameworks, libs = [], [], []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-framework" and index + 1 < len(tokens):
            frameworks.append(tokens[index + 1])
            index += 1
        elif token.startswith("-l"):
            libs.append(token)
        elif token.endswith(".a"):
            path = Path(token) if os.path.isabs(token) else build / token
            releaselib.require(path.is_file(), "link archive missing: " + token)
            archives.append(path)
        index += 1
    releaselib.require(archives, "the gpu_cube link uses no static archives")
    return archives, frameworks, libs


def assemble(build, source, package, archives):
    """Copy the install tree into ``package``; returns the list of declared relative paths."""
    declared = []

    def put(src, relative, executable=False):
        target = package / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, target)
        target.chmod(0o755 if executable else 0o644)
        declared.append(relative)

    aot = source / "core" / "aot"
    for path in sorted(aot.glob("*.py")):
        put(path, "compiler/" + path.name)
    capstone = source / "3rdparty" / "capstone" / "bindings" / "python" / "capstone"
    for path in sorted(capstone.glob("*.py")):
        put(path, "python/capstone/" + path.name)
    elftools = source / "3rdparty" / "pyelftools" / "elftools"
    for path in sorted(elftools.rglob("*.py")):
        put(path, "python/elftools/" + str(path.relative_to(elftools)))
    dylib = (build / "core" / "aot" / "capstone" / "libcapstone.dylib").resolve()
    releaselib.require(dylib.is_file(), "the build produced no libcapstone")
    put(dylib, "lib/libcapstone.dylib")
    seen = set()
    for archive in archives:
        if archive.name not in seen:
            seen.add(archive.name)
            put(archive, "runtime/lib/" + archive.name)
    put(source / "core" / "libs" / "prx" / "libc" / "include" / "NativeAotImage.hpp", "runtime/include/NativeAotImage.hpp")
    put(build / "core" / "libs" / "native" / "AnyPS5Utilities.metallib", "share/AnyPS5Utilities.metallib")
    put(source / "LICENSE", "share/LICENSE")
    for name, path in (("capstone", source / "3rdparty" / "capstone" / "LICENSE.TXT"),
                       ("pyelftools", source / "3rdparty" / "pyelftools" / "LICENSE"),
                       ("SDL2", source / "3rdparty" / "SDL2" / "LICENSE.txt")):
        if path.is_file():
            put(path, "share/licenses/%s-%s" % (name, path.name))
    launcher = package / "bin" / "native_aot"
    launcher.parent.mkdir(parents=True, exist_ok=True)
    launcher.write_text(LAUNCHER)
    launcher.chmod(0o755)
    declared.append("bin/native_aot")
    return declared


LAUNCHER = '''#!/usr/bin/env python3
"""Run the native AOT compiler from this package: bin/native_aot <args> (see --help)."""
import os
import sys

root = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
sys.path[:0] = [os.path.join(root, "compiler"), os.path.join(root, "python")]
os.environ["LIBCAPSTONE_PATH"] = os.path.join(root, "lib")

from compiler import main

main()
'''


def check_binaries(package):
    """The dylib and every archive are thin arm64 and load only system or package-relative libraries."""
    problems = []
    for path in sorted(Path(package).rglob("*")):
        if not path.is_file():
            continue
        relative = str(path.relative_to(package))
        if path.suffix in (".dylib", ".a"):
            architectures = subprocess.run(["lipo", "-archs", str(path)], capture_output=True, text=True)
            if architectures.returncode != 0 or architectures.stdout.split() != ["arm64"]:
                problems.append(relative + " is not a thin arm64 binary")
                continue
        if path.suffix == ".dylib":
            for dependency in build_package.load_commands(path):
                if not dependency.startswith(build_package.SYSTEM_PREFIXES) and dependency != path.name:
                    problems.append("%s loads %s" % (relative, dependency))
        elif path.suffix not in (".a", ".metallib") and build_package.macho_info(path)[0]:
            problems.append(relative + " is an unexpected Mach-O")
    releaselib.require(not problems, "package binaries: " + "; ".join(problems[:10]))


def write_manifest(package, version, revision, link, validation):
    files = {}
    for path in sorted(Path(package).rglob("*")):
        if path.is_file():
            files[str(path.relative_to(package))] = {"sha256": build_package.digest(path), "size": path.stat().st_size}
    manifest = {
        "schema": 1, "kind": "native", "arch": "arm64", "backend": BACKEND, "native_version": version,
        "executable": "bin/native_aot", "source_revisions": {"AnyPS5": revision},
        "contains_title_data": False,
        "requires": {"python": "3.9+", "clang": "Apple clang from the Xcode command line tools", "host": "Apple silicon macOS, native (not Rosetta)"},
        "link": link, "validation": validation, "files": files,
    }
    (Path(package) / "manifest.json").write_bytes(releaselib.encode_json(manifest))
    sums = "".join("%s  %s\n" % (build_package.digest(path), path.relative_to(package))
                   for path in sorted(Path(package).rglob("*")) if path.is_file())
    (Path(package) / "SHA256SUMS").write_text(sums)
    return build_package.digest(Path(package) / "manifest.json"), len(files)


def smoke(package, logs, log, name):
    """The packaged compiler starts from the package alone (clean environment, no checkout paths).
    ``-B``: running it must not write bytecode caches into the package it is about to ship."""
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "LD_", "PYTHON", "LIBCAPSTONE"))}
    path = build_package.run_logged(name, [sys.executable, "-I", "-B", package / "bin" / "native_aot", "--help"], logs, log, env=env)
    releaselib.require("usage:" in path.read_text(), name + ": the packaged compiler printed no usage")
    return "PASS"


def cube_summary(runs):
    """Frames compared and differing pixels from the newest acceptance.json under ``runs``."""
    found = sorted(Path(runs).glob("*/acceptance.json"))
    releaselib.require(found, "native_gpu_cube_check wrote no acceptance.json")
    comparison = json.loads(found[-1].read_text())["comparison"]["frames"]
    different = sum(frame["different_pixels"] for frame in comparison)
    releaselib.require(comparison and different == 0, "gpu_cube frames differ from the reference (%d pixels)" % different)
    return {"frames": len(comparison), "different_pixels": different}


def build(source, commit, version, work, log=print, jobs=None):
    source, work = Path(source).resolve(), Path(work)
    releaselib.require(re.fullmatch(r"[0-9a-f]{40}", commit or "") is not None, "--commit must be a full 40-hex SHA")
    releaselib.require(build_package.git(source, "rev-parse", "HEAD") == commit, "source checkout is not at --commit")
    releaselib.require(not build_package.git(source, "status", "--porcelain", "--untracked-files=all"),
                       "source checkout is not clean")
    releaselib.version_key(version)
    releaselib.require(not any(marker in str(work.resolve()).encode() for marker in build_package.private_markers()),
                       "--work must not contain the home path or user name (it is embedded in the binaries)")
    host = host_check()
    require_gui_session("the native release (hello_notify, gpu_cube)")
    work.mkdir(parents=True, exist_ok=False)
    work = work.resolve()
    logs = work / "logs"
    logs.mkdir()

    checkout = work / "source"
    log(json.dumps({"step": "clone", "commit": commit}))
    build_package.clone_source(source, commit, checkout, logs, log, submodules=NATIVE_SUBMODULES)
    revision = {"commit": commit, "tree": build_package.git(checkout, "rev-parse", "HEAD^{tree}"), "clean": True}

    build_dir = work / "build"
    log(json.dumps({"step": "build", "version": version}))
    build_package.run_logged("configure", ["cmake", "-S", checkout, "-B", build_dir, *CONFIGURE], logs, log,
                            timeout=STEP_TIMEOUTS["configure"])
    parallel = ["--parallel", str(jobs)] if jobs else ["--parallel"]
    build_package.run_logged("build", ["cmake", "--build", build_dir, *parallel, "--target", *BUILD_TARGETS], logs, log,
                            timeout=STEP_TIMEOUTS["build"])

    log(json.dumps({"step": "test", "filter": CTEST_FILTER}))
    ctest_log = build_package.run_logged("ctest", ["ctest", "--test-dir", build_dir, "-R", CTEST_FILTER, "-j", "4",
                                                   "--timeout", "120", "--output-on-failure"], logs, log,
                                       timeout=STEP_TIMEOUTS["ctest"])
    results = parse_ctest(ctest_log.read_text())
    check_ctest(results)
    require_gui_session("native_gpu_cube_check")  # the screen may have locked during the build
    log(json.dumps({"step": "gpu_cube", "target": "native_gpu_cube_check"}))
    build_package.run_logged("gpu-cube-check", ["cmake", "--build", build_dir, "--target", "native_gpu_cube_check"], logs, log,
                             timeout=STEP_TIMEOUTS["gpu-cube-check"])
    cube = cube_summary(build_dir / "core" / "libs" / "native" / "graphics-runs")
    validation = {"host": host, "ctest": {"filter": CTEST_FILTER, "passed": len(results), "failed": 0,
                                          "required": list(REQUIRED_TESTS)},
                  "native_gpu_cube_check": dict(cube, result="PASS"), "hello_notify": "PASS"}

    archives, frameworks, libs = link_closure(build_dir, logs, log)
    package = work / "package"
    package.mkdir()
    declared = assemble(build_dir, checkout, package, archives)
    link = {"archives": [archive.name for archive in archives], "frameworks": frameworks, "libs": libs}
    check_install_tree(package, declared)
    check_binaries(package)
    leaks = build_package.privacy_scan(package)
    releaselib.require(not leaks, "package bytes contain a private local path: " + ", ".join(leaks[:10]))
    smoke(package, logs, log, "smoke-staged")
    manifest_sha256, file_count = write_manifest(package, version, revision, link, validation)
    check_install_tree(package, declared)

    out = work / "out"
    out.mkdir()
    archive = build_package.write_zip(package, out / verify.asset_name("native", version))
    check_zip_tree(archive, package)
    roundtrip = verify.safe_extract(archive, work / "roundtrip" / "package")
    releaselib.require(build_package.digest(roundtrip / "manifest.json") == manifest_sha256, "round-tripped manifest differs")
    for name, record in json.loads((roundtrip / "manifest.json").read_text())["files"].items():
        path = roundtrip / name
        releaselib.require(path.is_file() and build_package.digest(path) == record["sha256"] and
                           path.stat().st_size == record["size"], "round-tripped file differs from the manifest: " + name)
    check_install_tree(roundtrip, declared)
    smoke(roundtrip, logs, log, "smoke-roundtrip")

    info = {"version": version, "commit": commit, "zip": str(archive), "zip_sha256": build_package.digest(archive),
            "zip_size": archive.stat().st_size, "package_manifest_sha256": manifest_sha256, "package_files": file_count,
            "backend": BACKEND, "kind": "native", "arch": "arm64", "validation": validation}
    (work / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")
    return info


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=None)
    args = parser.parse_args(argv)
    try:
        info = build(args.source, args.commit, args.version, args.work, jobs=args.jobs)
    except Skipped as skipped:
        print(json.dumps({"status": "SKIPPED", "reason": str(skipped)}), file=sys.stderr)
        return 0
    except (releaselib.ReleaseError, verify.VerificationError, subprocess.CalledProcessError, OSError, KeyError, ValueError) as error:
        print(json.dumps({"status": "FAILED", "error": str(error)}), file=sys.stderr)
        return 1
    print(json.dumps(info, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
