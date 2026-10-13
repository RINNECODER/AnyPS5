#!/usr/bin/env python3
"""Promote native engine builds between channels. Maintainer-triggered only (workflow_dispatch).

    python3 tools/release/native_promote.py beta   [--version V] [--skip-beta-age] [--dry-run]
    python3 tools/release/native_promote.py stable --version V --confirm [--dry-run]

Feeds are ``native-alpha.json``, ``native-beta.json`` and ``native-stable.json`` on the rolling
``channels`` release (product ``native``, channel ``alpha|beta|stable``), signed with the engine
signing key. MacPS refuses to install a ``native`` feed as an engine and the reverse.

beta: a native alpha build (``--version``, default the newest one newer than the current
native-beta) is promoted only if (a) it is at least 24 h old, unless ``--skip-beta-age``;
(b) no open ``native-regression`` issue names it or an earlier build; (c) its downloaded package
verifies against the signed record and its manifest says every gate held at build time (arm64 host
not under Rosetta, the AOT ctest suite including hello_notify, gpu_cube pixel-identical); and
(d) the packaged compiler starts. Phase 3's SDK-sample gate is exactly (c).

stable: only the build currently on native-beta, only with ``--confirm`` (the maintainer's explicit
approval that the Phase 4 gate holds), only when the workflow actor is the maintainer, and only
with no open ``native-regression`` issue. A gate that does not hold is a "hold" (exit 0); a
verification failure exits 1.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import native_package
import promote
import releaselib
import verify

PRODUCT = "native"
BETA_MIN_AGE = dt.timedelta(hours=24)
REGRESSION_LABEL = "native-regression"
say = promote.say


def check_manifest(package, record):
    """The package on disk is the signed one and its manifest records that every build gate held."""
    manifest = json.loads((Path(package) / "manifest.json").read_text())
    releaselib.require(manifest.get("kind") == "native" and manifest.get("arch") == "arm64", "manifest is not a native arm64 package")
    releaselib.require(manifest.get("contains_title_data") is False, "manifest does not declare a code-only package")
    revision = manifest.get("source_revisions", {}).get("AnyPS5", {})
    releaselib.require(revision.get("commit") == record["commit"] and manifest.get("native_version") == record["version"],
                       "manifest identity differs from the signed record")
    validation = manifest.get("validation", {})
    releaselib.require(validation.get("host", {}).get("proc_translated") == 0 and validation["host"].get("machine") == "arm64",
                       "built under Rosetta or on a non-arm64 host")
    ctest = validation.get("ctest", {})
    releaselib.require(ctest.get("failed") == 0 and ctest.get("passed", 0) >= native_package.MIN_TESTS and
                       set(native_package.REQUIRED_TESTS) <= set(ctest.get("required", [])), "native test gate did not hold")
    cube = validation.get("native_gpu_cube_check", {})
    releaselib.require(cube.get("result") == "PASS" and cube.get("different_pixels") == 0 and cube.get("frames", 0) > 0,
                       "gpu_cube gate did not hold")
    releaselib.require(validation.get("hello_notify") == "PASS", "hello_notify gate did not hold")
    native_package.check_install_tree(package, manifest["files"])
    for name, entry in manifest["files"].items():
        path = Path(package) / name
        releaselib.require(path.is_file() and verify.sha256_file(path) == entry["sha256"], "package file differs from the manifest: " + name)
    return manifest


def hold_reason_for_regressions(github, version):
    blocking = releaselib.blocking_regressions(github, version, REGRESSION_LABEL)
    return "open native-regression issue(s) " + ", ".join("#%d" % n for n in blocking) if blocking else None


def beta(github, now, args):
    github.ensure_labels()
    current = releaselib.current_feed(github, args.signer, PRODUCT, "beta")
    current_version = current["release"]["version"] if current else None
    builds = [(release, record) for release, record in releaselib.engine_releases(github, PRODUCT)
              if current is None or releaselib.version_key(record["version"]) > releaselib.version_key(current_version)]
    if args.version:
        builds = [item for item in builds if item[1]["version"] == args.version]
        if not builds:
            say("Beta hold: native %s is not a published build newer than native-beta %s." % (args.version, current_version))
            return 0
    elif not args.skip_beta_age:
        builds = [item for item in builds if now - releaselib.parse_rfc3339(item[1]["built_at"]) >= BETA_MIN_AGE]
    if not builds:
        say("Beta hold: no native alpha newer than native-beta %s is at least 24 h old." % current_version)
        return 0
    record = builds[0][1]
    version = record["version"]
    age = now - releaselib.parse_rfc3339(record["built_at"])
    if args.version and not args.skip_beta_age and age < BETA_MIN_AGE:
        say("Beta hold for %s: it has been on alpha for %s (needs 24 h; the maintainer may set skip_beta_age)." % (
            version, str(age).split(".")[0]))
        return 0
    reason = hold_reason_for_regressions(github, version)
    if reason:
        say("Beta hold for %s: %s." % (version, reason))
        return 0
    with tempfile.TemporaryDirectory(prefix="anyps5-native-promote-") as scratch:
        package = promote.fetch_package(record, scratch, "candidate")
        check_manifest(package, record)
        log = Path(scratch) / "logs"
        log.mkdir()
        native_package.smoke(package, log, print, "smoke")
    with tempfile.TemporaryDirectory(prefix="anyps5-feed-") as work:
        feed = releaselib.publish_feed(github, args.signer, PRODUCT, "beta", record, work, now)
    say(["Promoted native %s to native-beta (sequence %d)%s." % (version, feed["sequence"], " [dry run]" if github.dry_run else ""),
         "Gates: package verified against the signed record; manifest records arm64 host (proc_translated=0), the AOT "
         "ctest suite with hello_notify, and gpu_cube pixel-identical; packaged compiler starts."])
    return 0


def stable(github, now, args):
    if not args.confirm:
        say("Stable hold: the confirm input was not set. Promotion to native-stable needs the maintainer's explicit confirmation.")
        return 0
    actor = os.environ.get("GITHUB_ACTOR")
    if actor is not None and actor != releaselib.MAINTAINER:
        say("Stable refused: the workflow was triggered by %s, not @%s." % (actor, releaselib.MAINTAINER))
        return 0
    current_beta = releaselib.current_feed(github, args.signer, PRODUCT, "beta")
    current_stable = releaselib.current_feed(github, args.signer, PRODUCT, "stable")
    if current_beta is None:
        say("Stable refused: there is no native-beta yet.")
        return 0
    version = args.version
    release = github.release(PRODUCT + "-" + version)
    record = releaselib.verified_release_record(github, release, PRODUCT) if release else None
    refusal = None
    if record is None:
        refusal = "native-%s has no verified release.json" % version
    elif current_beta["release"] != record:
        # Only the build that is on native-beta now passed every beta gate.
        refusal = "native %s is not the current native-beta (%s)" % (version, current_beta["release"]["version"])
    elif current_stable and releaselib.version_key(version) <= releaselib.version_key(current_stable["release"]["version"]):
        refusal = "native %s is not newer than native-stable %s" % (version, current_stable["release"]["version"])
    else:
        refusal = hold_reason_for_regressions(github, version)
    if refusal:
        say("Stable promotion of native %s refused: %s." % (version, refusal))
        return 0
    with tempfile.TemporaryDirectory(prefix="anyps5-native-promote-") as scratch:
        check_manifest(promote.fetch_package(record, scratch, "candidate"), record)
    with tempfile.TemporaryDirectory(prefix="anyps5-feed-") as work:
        feed = releaselib.publish_feed(github, args.signer, PRODUCT, "stable", record, work, now)
    # Not --latest: the repository's "latest" release is the Rosetta engine's.
    github.write("mark native-%s as a full (stable) release" % version,
                 ["release", "edit", PRODUCT + "-" + version, "-R", releaselib.REPO, "--prerelease=false", "--latest=false"])
    say("Promoted native %s to native-stable (sequence %d)%s." % (version, feed["sequence"], " [dry run]" if github.dry_run else ""))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("beta", "stable"))
    parser.add_argument("--version", default="")
    parser.add_argument("--skip-beta-age", action="store_true")
    parser.add_argument("--confirm", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if args.version:
        releaselib.version_key(args.version)
    if args.action == "stable" and not args.version:
        parser.error("stable needs --version")
    github = releaselib.GitHub(dry_run=args.dry_run)
    args.signer = releaselib.Signer()
    now = releaselib.utc_now()
    try:
        return {"beta": beta, "stable": stable}[args.action](github, now, args)
    except (releaselib.ReleaseError, verify.VerificationError, subprocess.CalledProcessError, OSError, KeyError) as error:
        say("Native promotion failed: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
