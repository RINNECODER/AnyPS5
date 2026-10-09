#!/usr/bin/env python3
"""Promote engine builds between channels, per the release contract.

    python3 tools/release/promote.py labels             # create the release labels if missing
    python3 tools/release/promote.py beta               # alpha -> beta when every gate holds
    python3 tools/release/promote.py stable-candidate   # open "Promote engine-<v> to stable"
    python3 tools/release/promote.py stable [--issue N] # beta -> stable after maintainer approval

Add ``--dry-run`` to evaluate and print decisions without writing to GitHub.

beta: the newest verified alpha that is newer than the current beta and at least 24 h old
is promoted only if (a) the four required macOS checks passed on its commit, (b) MacPS
``EnginePackage.accept`` accepts its downloaded zip, (c) the PPSA04203 smoke (smoke.py,
title from ANYPS5_E2E_TITLE_DIR) reaches a boot stage >= the current beta's, run side by
side on this machine, and (d) no open ``regression`` issue is filed against it.

stable-candidate: once a beta has been on the beta channel for 48 h with no blocking
regression, open one issue labelled ``release:stable-candidate`` (and close superseded ones).

stable: only for an open issue labelled ``release:stable-candidate`` and
``release:approved`` whose most recent ``release:approved`` label event was made by
RINNECODER, and only for the build that is currently on beta. Agents must never add ``release:approved``.

A gate that does not hold is a "hold": it is reported and the run exits 0. Anything that
fails verification (a bad signature, a feed that would go backwards) exits 1.
"""
import argparse
import datetime as dt
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import macps_accept
import releaselib
import smoke
import verify

BETA_MIN_AGE = dt.timedelta(hours=24)
CANDIDATE_MIN_BETA_AGE = dt.timedelta(hours=48)
CANDIDATE_AUTHORS = ("github-actions[bot]", releaselib.MAINTAINER)
MARKER = re.compile(r"<!-- release-candidate: engine (" + verify.VERSION.pattern + r") -->")


def say(lines):
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if isinstance(lines, str):
        lines = [lines]
    if path:
        with open(path, "a") as out:
            out.write("\n".join(lines) + "\n")
    for line in lines:
        print(line)


def candidate_title(version):
    return "Promote engine-%s to stable" % version


def fetch_package(record, scratch, label):
    archive = Path(scratch) / (label + ".zip")
    verify.download(record["asset"]["url"], archive, record["asset"]["size"], record["asset"]["sha256"])
    package = verify.safe_extract(archive, Path(scratch) / label)
    releaselib.require(verify.sha256_file(package / "manifest.json") == record["package_manifest_sha256"],
                       "downloaded package manifest differs from the signed record")
    return package


def beta(github, now, args):
    github.ensure_labels()
    current = releaselib.current_feed(github, args.signer, "engine", "beta")
    current_version = current["release"]["version"] if current else None
    newer = [(release, record) for release, record in releaselib.engine_releases(github)
             if current is None or releaselib.version_key(record["version"]) > releaselib.version_key(current_version)]
    aged = [(release, record) for release, record in newer
            if now - releaselib.parse_rfc3339(record["built_at"]) >= BETA_MIN_AGE]
    if not aged:
        say("Beta hold: no alpha newer than beta %s is at least 24 h old (%d newer, younger)." % (current_version, len(newer)))
        return 0
    record = aged[0][1]
    version = record["version"]
    blocking = releaselib.blocking_regressions(github, version)
    if blocking:
        say("Beta hold for %s: open regression issue(s) %s." % (version, ", ".join("#%d" % n for n in blocking)))
        return 0
    missing = github.required_checks_passed(record["commit"])
    if missing:
        say("Beta hold for %s: required checks not passed on %s: %s." % (version, record["commit"], ", ".join(missing)))
        return 0
    title_dir = args.title_dir or os.environ.get("ANYPS5_E2E_TITLE_DIR")
    macps_repo = args.macps_repo or os.environ.get("ANYPS5_MACPS_REPO")
    if not title_dir or not macps_repo:
        say("Beta hold for %s: ANYPS5_E2E_TITLE_DIR and ANYPS5_MACPS_REPO must be set on the runner." % version)
        return 0
    with tempfile.TemporaryDirectory(prefix="anyps5-promote-") as scratch:
        package = fetch_package(record, scratch, "candidate")
        helper, macps_commit = macps_accept.build_helper(macps_repo, Path(scratch) / "helper")
        accepted = macps_accept.run_accept(helper, package, record["package_manifest_sha256"])
        releaselib.require(accepted["source_commit"] == record["commit"], "accepted package is from another commit")
        baseline_package = fetch_package(current["release"], scratch, "beta") if current else None

        def both():
            ran = {"candidate": smoke.run_smoke(package, title_dir)}
            if baseline_package is not None:
                ran["beta"] = smoke.run_smoke(baseline_package, title_dir)
            return ran

        try:
            results = smoke.with_title_lease(both)
        except RuntimeError as error:
            say("Beta hold for %s: title smoke could not run (%s)." % (version, error))
            return 0
    candidate_stage = results["candidate"]["stage"]
    beta_stage = results["beta"]["stage"] if "beta" in results else -1
    report = ["Smoke (furthest boot stage): candidate %s = %d %s%s" % (
        version, candidate_stage, results["candidate"]["stage_name"],
        "; beta %s = %d %s" % (current_version, beta_stage, results["beta"]["stage_name"]) if "beta" in results else "; no beta yet"),
        "Candidate smoke detail: code=%s message=%s" % (results["candidate"]["error_code"], results["candidate"]["message"]),
        "MacPS EnginePackage.accept: PASS (MacPS %s)" % macps_commit[:12]]
    if candidate_stage < beta_stage:
        say(["Beta hold for %s: smoke regressed below the current beta." % version] + report)
        return 0
    with tempfile.TemporaryDirectory(prefix="anyps5-feed-") as work:
        feed = releaselib.publish_feed(github, args.signer, "engine", "beta", record, work, now)
    say(["Promoted engine %s to beta (sequence %d)%s." % (version, feed["sequence"], " [dry run]" if github.dry_run else "")]
        + report)
    return 0


def candidate_issues(github):
    """Open stable-candidate issues from allowed authors, with the version from their marker."""
    found = []
    for issue in github.open_issues("release:stable-candidate"):
        match = MARKER.search(issue.get("body") or "")
        if issue["user"]["login"] in CANDIDATE_AUTHORS and match and issue["title"] == candidate_title(match.group(1)):
            found.append((issue, match.group(1)))
    return found


def stable_candidate(github, now, args):
    github.ensure_labels()
    current_beta = releaselib.current_feed(github, args.signer, "engine", "beta")
    current_stable = releaselib.current_feed(github, args.signer, "engine", "stable")
    if current_beta is None:
        say("No stable candidate: there is no beta yet.")
        return 0
    version = current_beta["release"]["version"]
    if current_stable and releaselib.version_key(version) <= releaselib.version_key(current_stable["release"]["version"]):
        say("No stable candidate: beta %s is not newer than stable." % version)
        return 0
    age = now - releaselib.parse_rfc3339(current_beta["updated_at"])
    if age < CANDIDATE_MIN_BETA_AGE:
        say("No stable candidate yet: beta %s has been on beta for %s (needs 48 h)." % (version, str(age).split(".")[0]))
        return 0
    blocking = releaselib.blocking_regressions(github, version)
    if blocking:
        say("No stable candidate: open regression issue(s) %s." % ", ".join("#%d" % n for n in blocking))
        return 0
    issues = candidate_issues(github)
    if any(candidate == version for _, candidate in issues):
        say("Stable candidate issue for %s is already open." % version)
        return 0
    for issue, candidate in issues:
        if issue["user"]["login"] == "github-actions[bot]":
            github.comment(issue["number"], "Superseded: engine %s is now the beta and the new stable candidate." % version)
            github.close_issue(issue["number"], "not planned")
    record = current_beta["release"]
    body = "\n".join([
        "<!-- release-candidate: engine %s -->" % version,
        "Engine `%s` (commit %s) has been on the **beta** channel since %s with no open `regression` issue against it."
        % (version, record["commit"], current_beta["updated_at"]),
        "",
        "- Build release: https://github.com/%s/releases/tag/engine-%s" % (releaselib.REPO, version),
        "- Zip sha256 `%s`, package manifest sha256 `%s`" % (record["asset"]["sha256"], record["package_manifest_sha256"]),
        "",
        "**Maintainer:** add the label `release:approved` to publish it to `engine-stable.json`.",
        "The promoter only acts when that label was added by @%s. **Agents must never add `release:approved`.**"
        % releaselib.MAINTAINER,
    ])
    github.write("open stable candidate issue for " + version,
                 ["issue", "create", "-R", releaselib.REPO, "--title", candidate_title(version),
                  "--label", "release:stable-candidate", "--body", body])
    return 0


def approval_actor(github, number):
    events = [event for event in github.issue_events(number)
              if event.get("event") == "labeled" and (event.get("label") or {}).get("name") == "release:approved"]
    return events[-1]["actor"]["login"] if events and events[-1].get("actor") else None


def stable(github, now, args):
    if args.issue:
        issue = github.issue(args.issue)
        match = MARKER.search(issue.get("body") or "")
        candidates = [(issue, match.group(1))] if match else []
        if not candidates:
            say("Issue #%d is not a release candidate issue." % args.issue)
            return 0
    else:
        candidates = candidate_issues(github)
    for issue, version in candidates:
        number = issue["number"]
        labels = {label["name"] for label in issue.get("labels", [])}
        if issue.get("state") != "open" or not {"release:stable-candidate", "release:approved"} <= labels:
            say("Issue #%d: not an open approved stable candidate; nothing to do." % number)
            continue
        refusal = None
        actor = approval_actor(github, number)
        if issue["user"]["login"] not in CANDIDATE_AUTHORS or issue["title"] != candidate_title(version):
            refusal = "the issue was not opened by the release promoter or the maintainer"
        elif actor != releaselib.MAINTAINER:
            refusal = "`release:approved` was last added by %s, not @%s" % (actor or "an unknown actor", releaselib.MAINTAINER)
        release = github.release("engine-" + version)
        record = releaselib.verified_release_record(github, release) if release else None
        current_stable = releaselib.current_feed(github, args.signer, "engine", "stable")
        current_beta = releaselib.current_feed(github, args.signer, "engine", "beta")
        if refusal is None and record is None:
            refusal = "engine-%s has no verified release.json" % version
        elif refusal is None and (current_beta is None or current_beta["release"] != record):
            # Only the build that is on beta now passed every beta gate; a version beta
            # skipped over, or one beta has moved past, is not eligible.
            refusal = "engine %s is not the current beta" % version
        elif refusal is None and current_stable and \
                releaselib.version_key(version) <= releaselib.version_key(current_stable["release"]["version"]):
            refusal = "engine %s is not newer than stable %s" % (version, current_stable["release"]["version"])
        if refusal is None:
            blocking = releaselib.blocking_regressions(github, version)
            if blocking:
                refusal = "open regression issue(s) " + ", ".join("#%d" % n for n in blocking)
        if refusal:
            say("Stable promotion of %s refused (issue #%d): %s." % (version, number, refusal))
            if args.issue:
                github.comment(number, "Stable promotion not performed: %s." % refusal)
            continue
        with tempfile.TemporaryDirectory(prefix="anyps5-feed-") as work:
            feed = releaselib.publish_feed(github, args.signer, "engine", "stable", record, work, now)
        github.write("mark engine-%s as a full (stable) release" % version,
                     ["release", "edit", "engine-" + version, "-R", releaselib.REPO, "--prerelease=false", "--latest"])
        github.comment(number, "Published engine %s to `engine-stable.json` (sequence %d), approved by @%s."
                       % (version, feed["sequence"], actor))
        github.close_issue(number)
        say("Promoted engine %s to stable (sequence %d)%s." % (version, feed["sequence"], " [dry run]" if github.dry_run else ""))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("labels", "beta", "stable-candidate", "stable"))
    parser.add_argument("--issue", type=int)
    parser.add_argument("--title-dir")
    parser.add_argument("--macps-repo")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    github = releaselib.GitHub(dry_run=args.dry_run)
    args.signer = releaselib.Signer()
    now = releaselib.utc_now()
    try:
        if args.action == "labels":
            github.ensure_labels()
            return 0
        return {"beta": beta, "stable-candidate": stable_candidate, "stable": stable}[args.action](github, now, args)
    except (releaselib.ReleaseError, verify.VerificationError, subprocess.CalledProcessError, OSError, KeyError) as error:
        say("Promotion failed: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
