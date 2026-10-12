#!/usr/bin/env python3
"""Build, verify and zip one Rosetta engine release package from a clean checkout.

    python3 tools/release/build_package.py --source <clean checkout with submodules> \
        --commit <40-hex HEAD> --version 2026.10.12.1 \
        --work <new directory without personal paths, e.g. /private/tmp/anyps5-release/x> \
        [--macps-repo <MacPS git repository>]

Steps (docs/RELEASES.md):

1. clone --source at --commit (and its submodules, from the source's own copies) into
   --work, so no byte of the build carries the runner's checkout path;
2. configure the x86_64 Rosetta build (-DANYPS5_ROSETTA_LIBS=ON
   -DCMAKE_OSX_ARCHITECTURES=x86_64) and build ``rosetta_engine`` (the libs, the relinker,
   anyps5_rosetta_run and the stub runtime) plus the launcher's unit test, and run it;
3. assemble the package: bin/anyps5_rosetta_run, bin/relinker, libs/ (x86_64 *.prx and
   AnyPS5Utilities.metallib), share/aps5_stub_runtime.dylib, share/LICENSE, manifest.json
   (schema 3) and SHA256SUMS; every Mach-O must be thin x86_64 (the relinker x86_64/arm64
   universal) and load only system libraries or package-relative ones;
4. refuse any package byte that holds the build user's home path or user name;
5. run the synthetic fixture title (tools/rosetta/tests/launcher_smoke.py) through the
   packaged launcher, cold and warm (never a retail title);
6. write a deterministic zip whose root is the package (manifest.json at the top), unzip
   it with the release extractor and run the fixture smoke again on those bytes;
7. with --macps-repo, run MacPS ``EnginePackage.accept`` on the package and on the
   round-tripped bytes, pinned to the manifest SHA-256.

Signing and publishing stay in alpha.py (release.json + Ed25519 signature). Prints one JSON
object describing the result; build-info.json in --work holds the same.
"""
import argparse
import getpass
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

import releaselib
import verify

ROOT = Path(__file__).resolve().parents[2]
ZIP_TIME = (1980, 1, 1, 0, 0, 0)
BACKEND = "Rosetta 2 direct execution (x86_64 relinked guest)"
EXECUTABLE = "bin/anyps5_rosetta_run"
SUBMODULES = ("3rdparty/SDL2", "3rdparty/SPIRV-Cross", "3rdparty/SPIRV-Headers", "3rdparty/Vulkan-Headers",
              "3rdparty/glslang", "3rdparty/LibAtrac9", "3rdparty/ffmpeg-core", "3rdparty/freetype",
              "3rdparty/libjpeg-turbo", "3rdparty/stb")
MAX_FILES = 512
SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/", "@rpath/", "@loader_path/", "@executable_path/")
MACHO_MAGIC_64 = b"\xcf\xfa\xed\xfe"
CPU_X86_64 = 0x01000007
CPU_ARM64 = 0x0100000C
# The relinker is a host tool the launcher runs natively: it ships universal (x86_64 and arm64).
UNIVERSAL_ALLOWED = {"bin/relinker"}


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def private_markers():
    markers = {str(Path.home()).encode()}
    user = getpass.getuser()
    if len(user) >= 6:  # short names would match unrelated bytes
        markers.add(user.encode())
    return markers


def privacy_scan(root, markers=None):
    """Relative paths of package files that contain a private marker."""
    markers = markers or private_markers()
    leaks = []
    for path in sorted(Path(root).rglob("*")):
        if path.is_file():
            data = path.read_bytes()
            if any(marker in data for marker in markers):
                leaks.append(str(path.relative_to(root)))
    return leaks


def write_zip(package, target):
    """Deterministic zip of the package contents; executables keep 0755, others 0644."""
    package = Path(package)
    entries = sorted(package.rglob("*"), key=lambda p: str(p.relative_to(package)))
    with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for path in entries:
            relative = str(path.relative_to(package))
            releaselib.require(not path.is_symlink(), "package contains a symbolic link: " + relative)
            if path.is_dir():
                info = zipfile.ZipInfo(relative + "/", ZIP_TIME)
                info.external_attr = (0o040755 << 16) | 0x10
                bundle.writestr(info, b"")
                continue
            info = zipfile.ZipInfo(relative, ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (0o100755 if os.access(path, os.X_OK) else 0o100644) << 16
            with path.open("rb") as source, bundle.open(info, "w") as out:
                shutil.copyfileobj(source, out, 1 << 20)
    return target


def accept(helper, package, manifest_sha256, cwd):
    result = subprocess.run([str(helper), str(package), manifest_sha256], cwd=cwd, capture_output=True, text=True,
                            env={k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "LD_"))})
    releaselib.require(result.returncode == 0, "MacPS EnginePackage.accept rejected the package: " + result.stderr.strip())
    return json.loads(result.stdout)


def digest(path):
    return verify.sha256_file(path)


def run_logged(name, command, logs, log, cwd=None, env=None):
    """Run a build step with its output in <logs>/<name>.log; show the tail on failure."""
    path = logs / (name + ".log")
    with open(path, "w") as stream:
        result = subprocess.run([str(part) for part in command], cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        text = path.read_text(errors="replace").replace(str(Path.home()), "~")
        log("--- %s failed (exit %d), last lines:\n%s" % (name, result.returncode, "\n".join(text.splitlines()[-40:])))
    releaselib.require(result.returncode == 0, "%s failed (exit %d)" % (name, result.returncode))
    return path


def clone_source(source, commit, target, logs, log):
    """A fresh clone of --source at --commit; submodules come from the source's checked-out copies."""
    run_logged("clone", ["git", "clone", "--quiet", "--no-hardlinks", "--no-checkout", source, target], logs, log)
    run_logged("checkout", ["git", "-C", target, "checkout", "--quiet", "--detach", commit], logs, log)
    names = git(target, "config", "-f", ".gitmodules", "--get-regexp", r"^submodule\..*\.path$").splitlines()
    by_path = {line.split()[1]: line.split()[0][len("submodule."):-len(".path")] for line in names}
    for path in SUBMODULES:
        releaselib.require(path in by_path and (Path(source) / path / ".git").exists(),
                           "source checkout lacks the submodule " + path)
        run_logged("submodule-config", ["git", "-C", target, "config", "submodule.%s.url" % by_path[path], Path(source) / path], logs, log)
    run_logged("submodules", ["git", "-C", target, "-c", "protocol.file.allow=always", "submodule", "update", "--init", "--quiet",
                              *SUBMODULES], logs, log)


def macho_info(path):
    """(is Mach-O, thin x86_64) for a file."""
    with open(path, "rb") as stream:
        header = stream.read(8)
    if header[:4] != MACHO_MAGIC_64:
        return header[:4] in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"), False
    return True, int.from_bytes(header[4:8], "little") == CPU_X86_64


def universal_architectures(path):
    """CPU types of a fat (universal) Mach-O, or None for anything else."""
    with open(path, "rb") as stream:
        data = stream.read(8 + 20 * 8)
    if data[:4] != b"\xca\xfe\xba\xbe":
        return None
    count = int.from_bytes(data[4:8], "big")
    return {int.from_bytes(data[8 + 20 * index:12 + 20 * index], "big") for index in range(min(count, 8))}


def load_commands(path):
    output = subprocess.check_output(["otool", "-L", str(path)], text=True)
    return re.findall(r"^\s+(\S.*?) \(compatibility version ", output, re.M)


def assemble(build, source, package):
    """Copy the engine files from the build's rosetta/ tree into the package layout."""
    rosetta = build / "rosetta"
    for directory in ("bin", "libs", "share"):
        (package / directory).mkdir(parents=True)
    shutil.copy2(rosetta / "bin" / "anyps5_rosetta_run", package / "bin" / "anyps5_rosetta_run")
    shutil.copy2(rosetta / "bin" / "relinker", package / "bin" / "relinker")
    shutil.copy2(rosetta / "share" / "aps5_stub_runtime.dylib", package / "share" / "aps5_stub_runtime.dylib")
    shutil.copyfile(source / "LICENSE", package / "share" / "LICENSE")
    libraries = sorted(path for path in (rosetta / "libs").iterdir() if path.is_file() and not path.is_symlink())
    releaselib.require(any(path.suffix == ".prx" for path in libraries) and (rosetta / "libs" / "AnyPS5Utilities.metallib").is_file(),
                       "the build produced no libs/*.prx or AnyPS5Utilities.metallib")
    for path in libraries:
        shutil.copy2(path, package / "libs" / path.name)
    for path in package.rglob("*"):
        if path.is_file():
            path.chmod(0o755 if path.parent.name == "bin" else 0o644)


def check_binaries(package):
    """Every Mach-O is thin x86_64 (bin/relinker may be x86_64/arm64 universal) and loads only system or
    package-relative libraries."""
    problems = []
    for path in sorted(package.rglob("*")):
        if not path.is_file():
            continue
        is_macho, x86_64 = macho_info(path)
        if not is_macho:
            continue
        relative = str(path.relative_to(package))
        universal = relative in UNIVERSAL_ALLOWED and universal_architectures(path) == {CPU_X86_64, CPU_ARM64}
        if not x86_64 and not universal:
            problems.append(relative + " is not a thin x86_64 Mach-O" + (" or an x86_64/arm64 universal one" if relative in UNIVERSAL_ALLOWED else ""))
            continue
        for dependency in load_commands(path):
            if not dependency.startswith(SYSTEM_PREFIXES) and dependency != path.name:
                problems.append("%s loads %s" % (relative, dependency))
    releaselib.require(not problems, "package binaries: " + "; ".join(problems[:10]))


def capabilities(package):
    output = subprocess.check_output([str(package / EXECUTABLE), "--capabilities-json"], text=True, timeout=30,
                                     env={k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "LD_"))})
    value = json.loads(output)
    releaselib.require(value.get("backend") == BACKEND and value.get("resource_root_argument") == "--resource-root" and
                       value.get("sce_module_argument") == "--sce-module" and value.get("schema_version") == 1 and
                       value.get("host_architecture") == "arm64" and value.get("guest_architecture") == "x86_64",
                       "launcher capabilities do not match the engine package contract")
    return value


def write_manifest(package, version, revision, caps):
    files = {str(path.relative_to(package)): {"sha256": digest(path), "size": path.stat().st_size}
             for path in sorted(package.rglob("*")) if path.is_file()}
    releaselib.require(0 < len(files) <= MAX_FILES and EXECUTABLE in files, "package file inventory out of range")
    manifest = {"schema_version": 3, "backend": BACKEND, "engine_version": version, "executable": EXECUTABLE,
                "architecture": "x86_64", "source_revisions": {"AnyPS5": revision}, "capabilities": caps,
                "validation": {"fixture": "tools/rosetta/tests/launcher_smoke.py (synthetic title, cold and warm relink)",
                               "result": "PASS"},
                "files": files}
    encoded = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    releaselib.require(len(encoded.encode()) <= 1048576, "manifest exceeds 1 MiB")
    (package / "manifest.json").write_text(encoded)
    (package / "SHA256SUMS").write_text("".join(digest(path) + "  " + str(path.relative_to(package)) + "\n"
                                                for path in sorted(package.rglob("*")) if path.is_file()))
    return digest(package / "manifest.json"), len(files)


def smoke(source, package, work, logs, log, name):
    """The synthetic fixture title through the packaged launcher (tools/rosetta/tests/launcher_smoke.py)."""
    path = run_logged(name, [sys.executable, source / "tools" / "rosetta" / "tests" / "launcher_smoke.py", package, work / name],
                      logs, log, env={k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "LD_"))})
    lines = path.read_text().splitlines()
    releaselib.require(lines and lines[-1] == "PASS", name + " did not pass")
    return json.loads(lines[-2])


def build(source, commit, macps_repo, version, work, macps_revision="HEAD", log=print, jobs=None):
    source, work = Path(source).resolve(), Path(work)
    releaselib.require(re.fullmatch(r"[0-9a-f]{40}", commit or "") is not None, "--commit must be a full 40-hex SHA")
    releaselib.require(git(source, "rev-parse", "HEAD") == commit, "source checkout is not at --commit")
    releaselib.require(not git(source, "status", "--porcelain", "--untracked-files=all"), "source checkout is not clean")
    releaselib.version_key(version)
    releaselib.require(not any(marker in str(work.resolve()).encode() for marker in private_markers()),
                       "--work must not contain the home path or user name (it is embedded in the binaries)")
    work.mkdir(parents=True, exist_ok=False)
    work = work.resolve()
    logs = work / "logs"
    logs.mkdir()

    checkout = work / "source"
    log(json.dumps({"step": "clone", "commit": commit}))
    clone_source(source, commit, checkout, logs, log)
    revision = {"commit": commit, "tree": git(checkout, "rev-parse", "HEAD^{tree}"), "clean": True}

    build_dir = work / "build"
    log(json.dumps({"step": "build", "version": version}))
    run_logged("configure", ["cmake", "-S", checkout, "-B", build_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                             "-DBUILD_TESTING=ON", "-DCMAKE_OSX_ARCHITECTURES=x86_64", "-DANYPS5_ROSETTA_LIBS=ON",
                             "-DANYPS5_ROSETTA_ENGINE_VERSION=" + version], logs, log)
    parallel = ["--parallel", str(jobs)] if jobs else ["--parallel"]
    run_logged("build", ["cmake", "--build", build_dir, *parallel, "--target", "rosetta_engine", "rosetta_launcher_tests"], logs, log)
    run_logged("unit-tests", ["ctest", "--test-dir", build_dir, "-R", "^rosetta_launcher_tests$", "--output-on-failure"], logs, log)

    package = work / "package"
    assemble(build_dir, checkout, package)
    check_binaries(package)
    caps = capabilities(package)
    releaselib.require(caps.get("engine_version") == version, "launcher reports another version")
    leaks = privacy_scan(package)
    releaselib.require(not leaks, "package bytes contain a private local path: " + ", ".join(leaks[:10]))
    log(json.dumps({"step": "smoke", "package": "staged"}))
    staged_smoke = smoke(checkout, package, work, logs, log, "smoke-staged")
    manifest_sha256, file_count = write_manifest(package, version, revision, caps)

    out = work / "out"
    out.mkdir()
    archive = write_zip(package, out / verify.asset_name("engine", version))
    roundtrip = verify.safe_extract(archive, work / "roundtrip" / "package")
    releaselib.require(digest(roundtrip / "manifest.json") == manifest_sha256, "round-tripped manifest differs")
    for name, record in json.loads((roundtrip / "manifest.json").read_text())["files"].items():
        path = roundtrip / name
        releaselib.require(path.is_file() and digest(path) == record["sha256"] and path.stat().st_size == record["size"],
                           "round-tripped file differs from the manifest: " + name)
    log(json.dumps({"step": "smoke", "package": "zip round trip"}))
    roundtrip_smoke = smoke(checkout, roundtrip, work, logs, log, "smoke-roundtrip")

    acceptance = {"macps": "NOT_RUN (no --macps-repo)"}
    macps_commit = None
    if macps_repo:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import macps_accept  # noqa: E402
        helper, macps_commit = macps_accept.build_helper(macps_repo, work / "macps", macps_revision)
        staged = macps_accept.run_accept(helper, package, manifest_sha256)
        again = macps_accept.run_accept(helper, roundtrip, manifest_sha256)
        for accepted in (staged, again):
            releaselib.require(accepted["manifest_sha256"] == manifest_sha256 and accepted["source_commit"] == commit and
                               accepted["backend"] == BACKEND, "MacPS accepted another package identity")
        acceptance = {"macps": "PASS", "build_dir": "PASS", "zip_roundtrip": "PASS"}

    info = {"version": version, "commit": commit, "zip": str(archive), "zip_sha256": digest(archive),
            "zip_size": archive.stat().st_size, "package_manifest_sha256": manifest_sha256, "package_files": file_count,
            "backend": BACKEND, "macps_commit": macps_commit, "acceptance": acceptance,
            "smoke": {"staged": staged_smoke, "zip_roundtrip": roundtrip_smoke}}
    (work / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")
    return info


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--macps-repo", type=Path, default=None)
    parser.add_argument("--macps-revision", default="HEAD")
    parser.add_argument("--version", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=None)
    args = parser.parse_args(argv)
    try:
        info = build(args.source, args.commit, args.macps_repo, args.version, args.work, args.macps_revision, jobs=args.jobs)
    except (releaselib.ReleaseError, verify.VerificationError, subprocess.CalledProcessError, OSError, KeyError, ValueError) as error:
        print(json.dumps({"status": "FAILED", "error": str(error)}), file=sys.stderr)
        return 1
    print(json.dumps(info, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
