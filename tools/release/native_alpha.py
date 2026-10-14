#!/usr/bin/env python3
"""Build and publish one native engine alpha: package -> zip -> release.json -> sign -> release -> feed.

    python3 tools/release/native_alpha.py --source <clean checkout with submodules> --commit <sha> \
        [--work-root /private/tmp/anyps5-release] [--dry-run]

Run by .github/workflows/native-release-alpha.yml for pushes to native/arm64, or locally with the
same arguments. Same flow as alpha.py, for the separate ``native`` product (tag ``native-<version>``,
feed ``native-alpha.json``, package ``kind: native`` / ``arch: arm64``):

1. Check the signing key matches the published public key.
2. Read and verify the current native-alpha.json. Skip if this commit is already the alpha or is
   not a descendant of it (an older run finishing late must never replace a newer alpha).
3. Pick the tag ``native-<YYYY.MM.DD>.<n>`` (n counts up per UTC day).
4. native_package.py: arm64 build, the AOT ctest suite, native_gpu_cube_check, the install-tree
   guard, package, zip, round trip.
5. Write release.json (schema 1), sign it, verify the signature independently.
6. ``gh release create`` the immutable prerelease titled "Native engine <version> (alpha)".
7. Write native-alpha.json with the next sequence, sign, upload, read back, verify.

``--dry-run`` does everything up to and including signing, in a scratch directory, and writes
nothing to GitHub (the gh wrapper only logs what it would do).
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

import alpha
import native_package
import releaselib
import verify

PRODUCT = "native"
DEFAULT_WORK_ROOT = Path("/private/tmp/anyps5-release")


def release_body(record, feed_url):
    return "\n".join([
        "Native engine alpha `%s` built from %s." % (record["version"], record["commit"]),
        "",
        "Experimental. The native ARM64 engine is separate from the Rosetta engine releases and is not a claim "
        "that any retail title runs.",
        "",
        "### Changes since the previous native alpha",
        record["notes"],
        "",
        "### Contents",
        "Code only: the offline x86-64 to ARM64 compiler (`core/aot`), the arm64 runtime archives and the Metal "
        "utility shaders. Nothing derived from a title is included; the user's Mac recompiles each installed title "
        "locally. See `docs/design/native-releases.md`.",
        "",
        "### Verify",
        "`release.json` is signed with Ed25519 (`release.json.sig`); the public key is in `docs/RELEASES.md`.",
        "```",
        "python3 tools/release/verify.py " + feed_url,
        "```",
        "Package manifest SHA-256: `%s`. Zip SHA-256: `%s`." % (record["package_manifest_sha256"], record["asset"]["sha256"]),
    ])


def run(args, log=print):
    github = releaselib.GitHub(dry_run=args.dry_run, log=log)
    signer = releaselib.Signer()
    releaselib.require(signer.public_key() == verify.PUBLIC_KEY_B64, "signing key does not match the published public key")
    previous = releaselib.current_feed(github, signer, PRODUCT, "alpha")
    previous_commit = previous["release"]["commit"] if previous else None
    if previous_commit == args.commit:
        alpha.summary(["Native alpha skipped: %s is already native-alpha (%s)." % (args.commit, previous["release"]["version"])])
        return 0
    if previous_commit is not None:
        status = github.compare(previous_commit, args.commit)["status"]
        if status != "ahead":
            alpha.summary(["Native alpha skipped: %s is %s of the current native alpha %s." % (args.commit, status, previous_commit)])
            return 0
    today = releaselib.utc_now()
    day = today.strftime("%Y.%m.%d.")
    tags = github.engine_tags(day, PRODUCT) + [r["tag_name"] for r in github.releases()]
    version = releaselib.next_version(tags, today, PRODUCT)
    tag = PRODUCT + "-" + version
    work = args.work_root / ("%s-%s" % (tag, args.commit[:12]))
    try:
        info = native_package.build(args.source, args.commit, version, work, log=log, jobs=args.jobs)
        archive = Path(info["zip"])
        out = archive.parent
        if previous_commit:
            notes = alpha.changelog(github, previous_commit, args.commit, args.source)
        else:
            subject = subprocess.check_output(["git", "-C", str(args.source), "log", "-1", "--format=%s", args.commit],
                                              text=True).strip()
            notes = "First native alpha. Head: " + subject
        record = releaselib.release_record(version, args.commit, releaselib.utc_now(), archive,
                                           info["package_manifest_sha256"], notes, product=PRODUCT)
        record_bytes = releaselib.encode_json(record)
        (out / "release.json").write_bytes(record_bytes)
        (out / "release.json.sig").write_bytes(signer.sign(record_bytes))
        verify.verify_signed((out / "release.json").read_bytes(), (out / "release.json.sig").read_bytes())
        feed_url = verify.DOWNLOAD_BASE + releaselib.CHANNELS_TAG + "/native-alpha.json"
        (out / "notes.md").write_text(release_body(record, feed_url) + "\n")
        github.write("create prerelease " + tag,
                     ["release", "create", tag, "-R", releaselib.REPO, "--target", args.commit, "--prerelease",
                      "--latest=false", "--title", "Native engine %s (alpha)" % version, "--notes-file", str(out / "notes.md"),
                      str(archive), str(out / "release.json"), str(out / "release.json.sig")])
        if not args.dry_run:
            published = github.release(tag)
            releaselib.require(published is not None and not published["draft"] and published["prerelease"],
                               "native build release was not published as a prerelease")
            releaselib.require(releaselib.verified_release_record(github, published, PRODUCT) == record,
                               "published release.json differs from the signed record")

        def still_descendant(current):
            if current is not None and current["release"]["commit"] != previous_commit:
                status = github.compare(current["release"]["commit"], args.commit)["status"]
                releaselib.require(status == "ahead", "native alpha moved to %s during this build; %s is %s of it"
                                   % (current["release"]["commit"], args.commit, status))

        feed = releaselib.publish_feed(github, signer, PRODUCT, "alpha", record, out, check_previous=still_descendant)
        if not args.dry_run:
            alpha.verify_public(feed_url, feed["sequence"])
        validation = info["validation"]
        alpha.summary(["%s native alpha `%s` (`%s`), feed sequence %d%s." % (
                           "Would publish" if args.dry_run else "Published", tag, args.commit, feed["sequence"],
                           " [dry run]" if args.dry_run else ""),
                       "- zip `%s` sha256 `%s` (%d bytes)" % (record["asset"]["name"], record["asset"]["sha256"], record["asset"]["size"]),
                       "- package manifest sha256 `%s`; %d native tests passed, gpu_cube %d frames pixel-identical" % (
                           record["package_manifest_sha256"], validation["ctest"]["passed"],
                           validation["native_gpu_cube_check"]["frames"])])
        return 0
    except native_package.Skipped as skipped:
        alpha.summary(["Native alpha skipped: %s. Nothing was published; re-run once the runner's screen is unlocked." % skipped])
        return 0
    finally:
        if not args.keep_work:
            releaselib.clean_tree(work)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--work-root", type=Path, default=DEFAULT_WORK_ROOT)
    parser.add_argument("--jobs", type=int, default=None)
    parser.add_argument("--keep-work", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="build, test, package and sign locally; write nothing to GitHub")
    args = parser.parse_args(argv)
    try:
        return run(args)
    except (releaselib.ReleaseError, verify.VerificationError, subprocess.CalledProcessError, OSError, KeyError) as error:
        print(json.dumps({"status": "FAILED", "error": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
