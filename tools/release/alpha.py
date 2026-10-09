#!/usr/bin/env python3
"""Build and publish one engine alpha: package -> zip -> release.json -> sign -> release -> feed.

    python3 tools/release/alpha.py --source <clean checkout with submodules> --commit <sha> \
        --macps-repo <MacPS git repository> [--work-root /private/tmp/anyps5-release] [--dry-run]

Run by .github/workflows/release-alpha.yml after the macOS Metal workflow passed for a
push to main, or locally with the same arguments. Steps:

1. Check the signing key matches the published public key and that the four required
   macOS checks passed on the commit (before any long build), for every entry point.
2. Read and verify the current engine-alpha.json. Skip if this commit is already the alpha
   or is not a descendant of it (an older run finishing late must never replace a newer alpha).
3. Pick the tag ``engine-<YYYY.MM.DD>.<n>`` (n counts up per UTC day).
4. build_package.py: fresh native-profile build, full CTest inventory, MacPS acceptance,
   private-path scan, zip, zip round-trip acceptance.
5. Write release.json (schema 1), sign it, verify the signature independently.
6. ``gh release create`` the immutable prerelease with the zip, release.json and .sig.
7. Write engine-alpha.json with the next sequence, sign, upload, read back, verify.
8. Verify the public feed URL end to end with verify.py (downloads the zip).
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import build_package
import releaselib
import verify

DEFAULT_WORK_ROOT = Path("/private/tmp/anyps5-release")
MAX_NOTES = 4000


def changelog(github, previous_commit, commit, source):
    if previous_commit is None:
        subject = subprocess.check_output(["git", "-C", str(source), "log", "-1", "--format=%s", commit], text=True).strip()
        return "First engine alpha. Head: " + subject
    comparison = github.compare(previous_commit, commit)
    lines = ["- " + item["commit"]["message"].splitlines()[0] for item in comparison.get("commits", [])]
    lines.reverse()  # newest first
    notes, kept = "", 0
    for line in lines:
        if len(notes) + len(line) + 1 > MAX_NOTES - 40:
            break
        notes += line + "\n"
        kept += 1
    if kept < len(lines):
        notes += "- ... and %d more commits\n" % (len(lines) - kept)
    return notes.rstrip("\n") or "No commit subjects since the previous alpha."


def release_body(record, feed_url):
    return "\n".join([
        "Engine alpha `%s` built from %s." % (record["version"], record["commit"]),
        "",
        "### Changes since the previous alpha",
        record["notes"],
        "",
        "### Verify",
        "`release.json` is signed with Ed25519 (`release.json.sig`); the public key is in `docs/RELEASES.md`.",
        "```",
        "python3 tools/release/verify.py " + feed_url,
        "```",
        "Package manifest SHA-256: `%s`. Zip SHA-256: `%s`." % (record["package_manifest_sha256"], record["asset"]["sha256"]),
        "",
        "Diagnostic engine package for MacPS; not a claim that any retail title runs.",
    ])


def summary(lines):
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a") as out:
            out.write("\n".join(lines) + "\n")
    for line in lines:
        print(line)


def verify_public(feed_url, sequence, attempts=6):
    for attempt in range(attempts):
        if verify.main([feed_url, "--min-sequence", str(sequence)]) == 0:
            return
        time.sleep(15 * (attempt + 1))
    raise releaselib.ReleaseError("public feed did not verify: " + feed_url)


def run(args, log=print):
    github = releaselib.GitHub(dry_run=args.dry_run, log=log)
    signer = releaselib.Signer()
    releaselib.require(signer.public_key() == verify.PUBLIC_KEY_B64, "signing key does not match the published public key")
    previous = releaselib.current_feed(github, signer, "engine", "alpha")
    previous_commit = previous["release"]["commit"] if previous else None
    if previous_commit == args.commit:
        summary(["Alpha skipped: %s is already engine-alpha (%s)." % (args.commit, previous["release"]["version"])])
        return 0
    missing = github.required_checks_passed(args.commit)
    releaselib.require(not missing, "required checks have not passed on %s: %s" % (args.commit, ", ".join(missing)))
    if previous_commit is not None:
        status = github.compare(previous_commit, args.commit)["status"]
        if status != "ahead":
            summary(["Alpha skipped: %s is %s of the current alpha %s." % (args.commit, status, previous_commit)])
            return 0
    today = releaselib.utc_now()
    day = today.strftime("%Y.%m.%d.")
    tags = github.engine_tags(day) + [r["tag_name"] for r in github.releases()]
    version = releaselib.next_version(tags, today)
    tag = "engine-" + version
    work = args.work_root / ("%s-%s" % (version, args.commit[:12]))
    try:
        info = build_package.build(args.source, args.commit, args.macps_repo, version, work, log=log)
        archive = Path(info["zip"])
        out = archive.parent
        record = releaselib.release_record(version, args.commit, releaselib.utc_now(), archive,
                                           info["package_manifest_sha256"],
                                           changelog(github, previous_commit, args.commit, args.source),
                                           min_macps=args.min_macps)
        record_bytes = releaselib.encode_json(record)
        (out / "release.json").write_bytes(record_bytes)
        (out / "release.json.sig").write_bytes(signer.sign(record_bytes))
        verify.verify_signed((out / "release.json").read_bytes(), (out / "release.json.sig").read_bytes())
        feed_url = verify.DOWNLOAD_BASE + releaselib.CHANNELS_TAG + "/engine-alpha.json"
        (out / "notes.md").write_text(release_body(record, feed_url) + "\n")
        github.write("create prerelease " + tag,
                     ["release", "create", tag, "-R", releaselib.REPO, "--target", args.commit, "--prerelease",
                      "--latest=false", "--title", "Engine %s (alpha)" % version, "--notes-file", str(out / "notes.md"),
                      str(archive), str(out / "release.json"), str(out / "release.json.sig")])
        if not args.dry_run:
            published = github.release(tag)
            releaselib.require(published is not None and not published["draft"] and published["prerelease"],
                               "build release was not published as a prerelease")
            releaselib.require(releaselib.verified_release_record(github, published) == record,
                               "published release.json differs from the signed record")
        def still_descendant(current):
            # Re-checked under the feed lock: another publisher may have moved alpha during the build.
            if current is not None and current["release"]["commit"] != previous_commit:
                status = github.compare(current["release"]["commit"], args.commit)["status"]
                releaselib.require(status == "ahead", "alpha moved to %s during this build; %s is %s of it"
                                   % (current["release"]["commit"], args.commit, status))

        feed = releaselib.publish_feed(github, signer, "engine", "alpha", record, out, check_previous=still_descendant)
        if not args.dry_run:
            verify_public(feed_url, feed["sequence"])
        summary(["Published engine alpha `%s` (`%s`), feed sequence %d%s." % (
                    tag, args.commit, feed["sequence"], " [dry run]" if args.dry_run else ""),
                 "- zip `%s` sha256 `%s` (%d bytes)" % (record["asset"]["name"], record["asset"]["sha256"], record["asset"]["size"]),
                 "- package manifest sha256 `%s`; MacPS acceptance: build dir PASS, zip round-trip PASS (MacPS %s)" % (
                    record["package_manifest_sha256"], info.get("macps_commit", "?")[:12])])
        return 0
    finally:
        if not args.keep_work:
            releaselib.clean_tree(work)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--macps-repo", type=Path, default=os.environ.get("ANYPS5_MACPS_REPO"))
    parser.add_argument("--work-root", type=Path, default=DEFAULT_WORK_ROOT)
    parser.add_argument("--min-macps", default=None)
    parser.add_argument("--keep-work", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="build and sign locally; write nothing to GitHub")
    args = parser.parse_args(argv)
    if not args.macps_repo:
        print("--macps-repo or ANYPS5_MACPS_REPO is required", file=sys.stderr)
        return 2
    try:
        return run(args)
    except (releaselib.ReleaseError, verify.VerificationError, subprocess.CalledProcessError, OSError, KeyError) as error:
        print(json.dumps({"status": "FAILED", "error": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
