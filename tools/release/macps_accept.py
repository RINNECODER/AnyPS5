#!/usr/bin/env python3
"""Run MacPS's production ``EnginePackage.accept`` on an engine zip or package directory.

    python3 tools/release/macps_accept.py <zip or dir> --macps-repo <MacPS git repository> \
        [--macps-revision HEAD] [--manifest-sha256 <hex>]

Builds LauncherCore from a fresh clone of the MacPS repository at the given revision and
links tools/accept_diagnostic_package.swift against it (the same helper
prepare_diagnostic_engine.py uses), then accepts the package pinned to its manifest
SHA-256. A zip is unpacked with the safe extractor from verify.py first. Prints the
acceptance record; exit status 0 only when MacPS accepted the package.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import verify

HELPER_SOURCE = Path(__file__).resolve().parents[1] / "accept_diagnostic_package.swift"


def build_helper(macps_repo, work, revision="HEAD"):
    work = Path(work)
    macps = work / "macps-source"
    subprocess.run(["git", "clone", "--quiet", "--no-hardlinks", "--no-checkout", str(macps_repo), str(macps)], check=True)
    commit = subprocess.check_output(["git", "-C", str(macps_repo), "rev-parse", revision + "^{commit}"], text=True).strip()
    subprocess.run(["git", "-C", str(macps), "checkout", "--quiet", "--detach", commit], check=True)
    scratch = work / "swift-build"
    common = ["swift", "build", "--package-path", str(macps), "--scratch-path", str(scratch), "--configuration", "debug"]
    subprocess.run(common + ["--target", "LauncherCore", "--jobs", "2", "-Xswiftc", "-strict-concurrency=complete"],
                   check=True, stdout=subprocess.DEVNULL)
    binary = Path(subprocess.check_output(common + ["--show-bin-path"], text=True).strip())
    combined = binary / "LauncherCore.o"
    if combined.is_file():
        objects, modules = [combined], binary
    else:
        objects, modules = sorted((binary / "LauncherCore.build").glob("*.o")), binary / "Modules"
    if not objects:
        raise RuntimeError("fresh LauncherCore objects missing")
    helper = work / "accept-diagnostic-package"
    subprocess.run(["swiftc", "-parse-as-library", "-strict-concurrency=complete", "-I", str(modules),
                    str(HELPER_SOURCE), *map(str, objects), "-o", str(helper)], check=True)
    return helper, commit


def run_accept(helper, package, manifest_sha256):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "LD_"))}
    with tempfile.TemporaryDirectory(prefix="anyps5-accept-cwd-") as cwd:
        result = subprocess.run([str(helper), str(package), manifest_sha256], cwd=cwd, env=env,
                                capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("MacPS EnginePackage.accept rejected the package: " + result.stderr.strip())
    return json.loads(result.stdout)


def accept_path(path, macps_repo, revision="HEAD", manifest_sha256=None):
    path = Path(path)
    with tempfile.TemporaryDirectory(prefix="anyps5-accept-") as scratch:
        scratch = Path(scratch)
        package = verify.safe_extract(path, scratch / "package") if path.is_file() else path
        expected = manifest_sha256 or verify.sha256_file(package / "manifest.json")
        if verify.sha256_file(package / "manifest.json") != expected:
            raise RuntimeError("package manifest SHA-256 differs from the expected value")
        helper, commit = build_helper(macps_repo, scratch / "helper", revision)
        record = run_accept(helper, package, expected)
    record["macps_commit"] = commit
    record.pop("package_root", None)
    record.pop("engine_executable", None)
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("package", type=Path)
    parser.add_argument("--macps-repo", type=Path, default=os.environ.get("ANYPS5_MACPS_REPO"))
    parser.add_argument("--macps-revision", default="HEAD")
    parser.add_argument("--manifest-sha256")
    args = parser.parse_args(argv)
    if not args.macps_repo:
        print("--macps-repo or ANYPS5_MACPS_REPO is required", file=sys.stderr)
        return 2
    try:
        record = accept_path(args.package, args.macps_repo, args.macps_revision, args.manifest_sha256)
    except (RuntimeError, OSError, subprocess.CalledProcessError, verify.VerificationError) as error:
        print(json.dumps({"status": "REJECTED", "reason": str(error)}), file=sys.stderr)
        return 1
    record["status"] = "ACCEPTED"
    print(json.dumps(record, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
