#!/usr/bin/env python3
"""Build, accept and zip one engine release package from a clean checkout.

    python3 tools/release/build_package.py --source <clean checkout with submodules> \
        --commit <40-hex HEAD> --macps-repo <MacPS git repository> --version 2026.10.09.1 \
        --work <new directory without personal paths, e.g. /private/tmp/anyps5-release/x>

The release successor of the diagnostic package flow. It runs the existing
tools/prepare_diagnostic_engine.py ``--profile native`` unchanged (fresh clone and build,
the full declared native CTest inventory, packaging, relocated fixture runs and a fresh
MacPS ``EnginePackage.accept`` with negative controls), then:

* refuses any package byte that contains the build user's home path or user name, so a
  public release never leaks a local path (the tools are run from a copy under --work
  for the same reason);
* writes a deterministic zip whose root is the package directory itself
  (``manifest.json``, ``bin/``, ``lib/``, ``fixtures/``, ``SHA256SUMS``), with Unix modes;
* unzips that zip with ``ditto`` into a fresh directory and runs MacPS acceptance again on
  the round-tripped bytes, pinned to the manifest SHA-256.

Prints one JSON object describing the result; build-info.json in --work holds the same.
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

TOOL_FILES = ("prepare_diagnostic_engine.py", "diagnostic_package.py",
              "native_diagnostic_controls.cmake", "accept_diagnostic_package.swift")
ZIP_TIME = (1980, 1, 1, 0, 0, 0)


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


def failed_log_tail(receipt, log, lines=40):
    """Show the failing command's last output before the work tree is removed (home path redacted)."""
    failed = [item for item in receipt.get("commands", []) if item.get("exit_code") != item.get("expected_exit_code")]
    if not failed:
        return
    item = failed[-1]
    log("prepare_diagnostic_engine command failed: %s (exit %s)" % (item["name"], item["exit_code"]))
    for stream in ("stdout", "stderr"):
        path = Path(item["logs"][stream]["path"])
        if path.is_file():
            text = path.read_text(errors="replace").replace(str(Path.home()), "~")
            log("--- %s (last %d lines)\n%s" % (stream, lines, "\n".join(text.splitlines()[-lines:])))


def build(source, commit, macps_repo, version, work, macps_revision="HEAD", log=print, attempts=3):
    source, work = Path(source).resolve(), Path(work)
    releaselib.require(re.fullmatch(r"[0-9a-f]{40}", commit or "") is not None, "--commit must be a full 40-hex SHA")
    releaselib.require(git(source, "rev-parse", "HEAD") == commit, "source checkout is not at --commit")
    releaselib.require(not git(source, "status", "--porcelain", "--untracked-files=all"), "source checkout is not clean")
    releaselib.version_key(version)
    releaselib.require(not any(marker in str(work.resolve()).encode() for marker in private_markers()),
                       "--work must not contain the home path or user name (it is embedded in the binaries)")
    work.mkdir(parents=True, exist_ok=False)
    tools = work / "tools"
    tools.mkdir()
    for name in TOOL_FILES:
        shutil.copyfile(source / "tools" / name, tools / name)
    macps = work / "macps-source"
    subprocess.run(["git", "clone", "--quiet", "--no-hardlinks", "--no-checkout", str(macps_repo), str(macps)], check=True)
    macps_commit = git(macps_repo, "rev-parse", macps_revision + "^{commit}")
    subprocess.run(["git", "-C", str(macps), "checkout", "--quiet", "--detach", macps_commit], check=True)
    engine = work / "engine"
    for attempt in range(1, attempts + 1):
        # A second attempt only absorbs the documented AppKit focus flake in relocated window
        # tests; a real failure fails both fresh builds.
        releaselib.clean_tree(engine)
        log(json.dumps({"step": "prepare_diagnostic_engine", "profile": "native", "attempt": attempt,
                        "commit": commit, "macps_commit": macps_commit}))
        result = subprocess.run([sys.executable, str(tools / "prepare_diagnostic_engine.py"), "--source", str(source),
                                 "--revision", commit, "--macps-source", str(macps), "--macps-revision", macps_commit,
                                 "--output", str(engine), "--dependency-cache", str(source), "--profile", "native"],
                                cwd=str(work), check=False)
        receipt_path = engine / "receipt.json"
        receipt = json.loads(receipt_path.read_text()) if receipt_path.is_file() else {}
        if result.returncode == 0 and receipt.get("status") == "PASS":
            break
        failed_log_tail(receipt, log)
        releaselib.require(attempt < attempts, "prepare_diagnostic_engine failed: " +
                           str(receipt.get("failure", "exit %d" % result.returncode)))
    package = Path(receipt["package"]["package"])
    manifest_sha256 = receipt["package"]["manifest_sha256"]
    releaselib.require(receipt["strict_acceptance"]["source_commit"] == commit, "accepted package is from another commit")
    releaselib.require(verify.sha256_file(package / "manifest.json") == manifest_sha256, "manifest changed after acceptance")
    leaks = privacy_scan(package)
    releaselib.require(not leaks, "package bytes contain a private local path: " + ", ".join(leaks[:10]))
    out = work / "out"
    out.mkdir()
    archive = write_zip(package, out / verify.asset_name("engine", version))
    roundtrip = work / "roundtrip"
    roundtrip.mkdir()
    subprocess.run(["/usr/bin/ditto", "-x", "-k", str(archive), str(roundtrip / "package")], check=True)
    accepted = accept(engine / "accept-diagnostic-package", roundtrip / "package", manifest_sha256, roundtrip)
    releaselib.require(accepted["manifest_sha256"] == manifest_sha256 and accepted["source_commit"] == commit and
                       accepted["ps5_game_runtime_ready"] is False, "round-tripped package identity mismatch")
    info = {"version": version, "commit": commit, "zip": str(archive), "zip_sha256": verify.sha256_file(archive),
            "zip_size": archive.stat().st_size, "package_manifest_sha256": manifest_sha256,
            "package_files": len(receipt["package"]["artifacts"]), "macps_commit": macps_commit,
            "native_ctest_results": len(receipt.get("ctest_results", [])),
            "acceptance": {"build_dir": "PASS", "zip_roundtrip": "PASS", "backend": accepted["backend"],
                           "engine_commit": accepted["engine_commit"]}}
    (work / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")
    return info


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--macps-repo", type=Path, required=True)
    parser.add_argument("--macps-revision", default="HEAD")
    parser.add_argument("--version", required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        info = build(args.source, args.commit, args.macps_repo, args.version, args.work, args.macps_revision)
    except (releaselib.ReleaseError, subprocess.CalledProcessError, OSError, KeyError) as error:
        print(json.dumps({"status": "FAILED", "error": str(error)}), file=sys.stderr)
        return 1
    print(json.dumps(info, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
