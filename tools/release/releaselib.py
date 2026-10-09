"""Shared pieces of the engine release-channel pipeline.

Contract: anyps5-ops release-contract (docs/RELEASES.md summarises it). Everything that
writes to GitHub goes through ``GitHub``; everything that signs goes through ``Signer``.
Every signed document is re-verified with the independent RFC 8032 verifier in
verify.py before it is uploaded, and every published feed is read back and verified
before its sequence is trusted.
"""
import datetime as _dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile

import verify

REPO = verify.REPO
CHANNELS_TAG = "channels"
CHANNELS = ("alpha", "beta", "stable")
REQUIRED_CHECKS = ("macOS CPU TCG", "macOS native runner", "macOS shader bridge", "macOS guest replay")
MAINTAINER = "RINNECODER"
LABELS = {
    "regression": ("d73a4a", "A released engine/app version regressed; blocks beta and stable promotion"),
    "release:stable-candidate": ("0e8a16", "Opened by the release promoter: a beta proposed for stable"),
    "release:approved": ("5319e7", "Maintainer-only stable approval. Agents must never add this label."),
}
DEFAULT_KEY = Path.home() / "Documents/ChatGPT/metal/anyps5-ops/keys/release-signing.key"


class ReleaseError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise ReleaseError(message)


def utc_now():
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0)


def rfc3339(moment):
    return moment.astimezone(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_rfc3339(text):
    return _dt.datetime.strptime(text[:19], "%Y-%m-%dT%H:%M:%S").replace(tzinfo=_dt.timezone.utc)


def version_key(version):
    require(verify.VERSION.fullmatch(version or "") is not None, "malformed version: %r" % (version,))
    return tuple(int(part) for part in version.split("."))


def next_version(existing_tags, today):
    """``YYYY.MM.DD.n``: n counts up per UTC day from 1, past every existing engine tag of that day."""
    prefix = today.strftime("%Y.%m.%d") + "."
    used = [int(tag[len("engine-") + len(prefix):]) for tag in existing_tags
            if tag.startswith("engine-" + prefix) and tag[len("engine-") + len(prefix):].isdigit()]
    return prefix + str(max(used, default=0) + 1)


def sha256_file(path):
    return verify.sha256_file(path)


def encode_json(value):
    """The exact bytes that get signed and published."""
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def release_record(version, commit, built_at, asset_path, manifest_sha256, notes, channel="alpha", min_macps=None):
    name = verify.asset_name("engine", version)
    require(Path(asset_path).name == name, "asset file must be named " + name)
    record = {
        "schema": 1, "product": "engine", "version": version, "channel_at_build": channel, "commit": commit,
        "built_at": rfc3339(built_at), "min_macps": min_macps,
        "asset": {"name": name, "url": verify.DOWNLOAD_BASE + "engine-" + version + "/" + name,
                  "sha256": sha256_file(asset_path), "size": Path(asset_path).stat().st_size},
        "package_manifest_sha256": manifest_sha256, "notes": notes,
    }
    return verify.check_release(record, "engine")


def feed_document(product, channel, sequence, record, updated_at):
    feed = {"schema": 1, "product": product, "channel": channel, "sequence": sequence,
            "updated_at": rfc3339(updated_at), "release": record}
    return verify.check_feed(feed, product, channel)


def next_sequence(previous_feed, record):
    """Strictly increasing sequence; a feed never moves to an older or equal build."""
    if previous_feed is None:
        return 1
    previous = previous_feed["release"]["version"]
    require(version_key(record["version"]) > version_key(previous),
            "refusing to move the %s feed from %s to %s (not newer)" % (previous_feed["channel"], previous, record["version"]))
    return previous_feed["sequence"] + 1


# ---- signing ---------------------------------------------------------------------------
class Signer:
    """Signs with CryptoKit via signer.swift; the key file is only ever read, never echoed."""

    def __init__(self, key_path=None, expected_public_key=verify.PUBLIC_KEY_B64, state_dir=None):
        self.key_path = Path(key_path or os.environ.get("ANYPS5_RELEASE_KEY") or DEFAULT_KEY)
        self.expected_public_key = expected_public_key
        # The feed journal lives with the key, on the only machine that can sign.
        self.state_dir = Path(state_dir or os.environ.get("ANYPS5_RELEASE_STATE") or
                              self.key_path.parent.parent / "state" / "release-feeds")
        self._binary = None

    def check_key_file(self):
        try:
            info = self.key_path.lstat()
        except OSError:
            raise ReleaseError("release signing key is not readable at the configured path") from None
        require(stat.S_ISREG(info.st_mode), "release signing key must be a regular file, not a link")
        require(info.st_uid == os.getuid(), "release signing key must be owned by the signing user")
        require(stat.S_IMODE(info.st_mode) & 0o077 == 0, "release signing key must be mode 0600")

    def binary(self):
        if self._binary:
            return self._binary
        source = Path(__file__).with_name("signer.swift")
        digest = hashlib.sha256(source.read_bytes()).hexdigest()[:16]
        target = Path(tempfile.gettempdir()) / "anyps5-release-signer" / digest / "signer"
        if not target.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = target.with_name("signer.%d.partial" % os.getpid())
            subprocess.run(["swiftc", "-O", str(source), "-o", str(partial)], check=True,
                           stdout=subprocess.DEVNULL)
            os.replace(partial, target)
        self._binary = target
        return target

    def public_key(self):
        self.check_key_file()
        result = subprocess.run([str(self.binary()), "public", str(self.key_path)], capture_output=True, check=False)
        require(result.returncode == 0, "signer could not derive the public key: " + result.stderr.decode(errors="replace").strip())
        return result.stdout.decode().strip()

    def sign(self, data):
        """Return the ``.sig`` file bytes for ``data`` after independently verifying them."""
        self.check_key_file()
        require(self.public_key() == self.expected_public_key,
                "release signing key does not match the published public key")
        with tempfile.TemporaryDirectory(prefix="anyps5-sign-") as scratch:
            message = Path(scratch) / "message"
            message.write_bytes(data)
            result = subprocess.run([str(self.binary()), "sign", str(self.key_path), str(message)],
                                    capture_output=True, check=False)
        require(result.returncode == 0, "signing failed: " + result.stderr.decode(errors="replace").strip())
        signature = result.stdout
        verify.verify_signed(data, signature, verify.base64.b64decode(self.expected_public_key))
        return signature


# ---- GitHub ----------------------------------------------------------------------------
class GitHub:
    """Thin ``gh`` wrapper pinned to the fork. Never touches the upstream repository."""

    def __init__(self, repo=REPO, dry_run=False, log=print):
        require(repo == REPO, "release tooling only publishes to " + REPO)
        self.repo, self.dry_run, self.log = repo, dry_run, log

    def _run(self, args, data=None, check=True):
        result = subprocess.run(["gh", *args], input=data, capture_output=True, check=False)
        if check and result.returncode != 0:
            raise ReleaseError("gh %s failed: %s" % (args[0] if args[0] != "api" else "api " + args[-1],
                                                    result.stderr.decode(errors="replace").strip()[:500]))
        return result

    def api(self, path, method="GET", accept=None, paginate_items=False, allow_missing=False):
        args = ["api", "-X", method]
        if accept:
            args += ["-H", "Accept: " + accept]
        if paginate_items:
            args += ["--paginate", "--jq", ".[] | tojson"]
        args.append(path)
        result = self._run(args, check=False)
        if result.returncode != 0:
            if allow_missing and b"HTTP 404" in result.stderr:
                return None
            raise ReleaseError("gh api %s %s failed: %s" % (method, path, result.stderr.decode(errors="replace").strip()[:500]))
        if accept == "application/octet-stream":
            return result.stdout
        if paginate_items:
            return [json.loads(line) for line in result.stdout.decode().splitlines() if line.strip()]
        return json.loads(result.stdout or b"null")

    def write(self, description, args):
        if self.dry_run:
            self.log("DRY-RUN would " + description)
            return None
        self.log(description)
        return self._run(args)

    # releases
    def releases(self):
        return self.api("repos/%s/releases?per_page=100" % self.repo, paginate_items=True)

    def release(self, tag):
        return self.api("repos/%s/releases/tags/%s" % (self.repo, tag), allow_missing=True)

    def engine_tags(self, day_prefix):
        refs = self.api("repos/%s/git/matching-refs/tags/engine-%s" % (self.repo, day_prefix)) or []
        return [ref["ref"][len("refs/tags/"):] for ref in refs]

    def asset_bytes(self, release, name):
        for asset in release.get("assets", []):
            if asset["name"] == name:
                return self.api("repos/%s/releases/assets/%d" % (self.repo, asset["id"]), accept="application/octet-stream")
        return None

    def ensure_channels_release(self):
        if self.release(CHANNELS_TAG) is None:
            self.write("create the rolling '%s' release" % CHANNELS_TAG,
                       ["release", "create", CHANNELS_TAG, "-R", self.repo, "--title", "Release channels",
                        "--latest=false", "--notes",
                        "Rolling, signed channel feeds (engine-*.json, macps-*.json and their .sig files). "
                        "Not a build: see docs/RELEASES.md. Assets are replaced in place."])

    def upload(self, tag, paths):
        self.write("upload %s to release %s" % (", ".join(Path(p).name for p in paths), tag),
                   ["release", "upload", tag, "-R", self.repo, "--clobber", *[str(p) for p in paths]])

    # issues and labels
    def ensure_labels(self):
        existing = {label["name"] for label in self.api("repos/%s/labels?per_page=100" % self.repo, paginate_items=True)}
        for name, (color, description) in LABELS.items():
            if name not in existing:
                self.write("create label " + name, ["label", "create", name, "-R", self.repo, "--color", color,
                                                    "--description", description])

    def open_issues(self, label):
        items = self.api("repos/%s/issues?state=open&per_page=100&labels=%s" % (self.repo, label), paginate_items=True)
        return [item for item in items if "pull_request" not in item]

    def issue(self, number):
        return self.api("repos/%s/issues/%d" % (self.repo, number))

    def issue_events(self, number):
        return self.api("repos/%s/issues/%d/events?per_page=100" % (self.repo, number), paginate_items=True)

    def comment(self, number, body):
        self.write("comment on issue #%d" % number, ["issue", "comment", str(number), "-R", self.repo, "--body", body])

    def close_issue(self, number, reason="completed"):
        self.write("close issue #%d" % number, ["issue", "close", str(number), "-R", self.repo, "--reason", reason])

    # commits and checks
    def required_checks_passed(self, commit):
        runs = self.api("repos/%s/commits/%s/check-runs?per_page=100" % (self.repo, commit))["check_runs"]
        passed = {run["name"] for run in runs if run.get("status") == "completed" and run.get("conclusion") == "success"}
        return [name for name in REQUIRED_CHECKS if name not in passed]

    def compare(self, base, head):
        return self.api("repos/%s/compare/%s...%s" % (self.repo, base, head))


def verified_release_record(github, release):
    """Read and verify ``release.json`` + ``.sig`` of a build release; None when absent."""
    tag = release["tag_name"]
    data, signature = github.asset_bytes(release, "release.json"), github.asset_bytes(release, "release.json.sig")
    if data is None or signature is None:
        return None
    verify.verify_signed(data, signature)
    record = verify.check_release(verify.parse_json(data), "engine")
    require("engine-" + record["version"] == tag, "release.json version does not match tag " + tag)
    return record


def read_feed(github, product, channel, channels_release=None):
    """The currently published feed, verified, or None if it was never published.

    Raises when the published pair exists but does not verify.
    """
    release = channels_release if channels_release is not None else github.release(CHANNELS_TAG)
    if release is None:
        return None
    name = "%s-%s.json" % (product, channel)
    data, signature = github.asset_bytes(release, name), github.asset_bytes(release, name + ".sig")
    if data is None and signature is None:
        return None
    require(data is not None and signature is not None, "published %s is missing its data or signature" % name)
    return verify.verify_feed_bytes(name, data, signature)


def read_journal(signer, product, channel):
    """The last feed this signer published (verified), or None."""
    name = "%s-%s.json" % (product, channel)
    path = signer.state_dir / (name + ".journal")
    if not path.exists():
        return None
    entry = json.loads(path.read_text())
    return verify.verify_feed_bytes(name, verify.base64.b64decode(entry["data"]), entry["sig"].encode())


def write_journal(signer, name, data, signature):
    """One file per feed, replaced atomically, so the journal pair can never be half-written."""
    path = signer.state_dir / (name + ".journal")
    partial = path.with_name(path.name + ".partial")
    partial.write_text(json.dumps({"data": verify.base64.b64encode(data).decode(), "sig": signature.decode()}) + "\n")
    os.replace(partial, path)


def current_feed(github, signer, product, channel):
    """The newest trustworthy feed: the published one or the signer's journal, whichever is later.

    The journal is the durable sequence watermark. It lets a publish recover from a
    half-replaced public pair (json and sig are separate uploads) and stops a replayed old
    public feed from resetting the sequence. With no journal, an unverifiable public feed
    still stops the pipeline.
    """
    journal = read_journal(signer, product, channel)
    try:
        published = read_feed(github, product, channel)
    except verify.VerificationError:
        if journal is None:
            raise
        published = None
    candidates = [feed for feed in (published, journal) if feed is not None]
    return max(candidates, key=lambda feed: feed["sequence"]) if candidates else None


class _FeedLock:
    """Machine-wide lock so CI jobs and local runs never allocate sequences concurrently."""

    def __init__(self, directory):
        self.directory = Path(directory)

    def __enter__(self):
        import fcntl
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.handle = open(self.directory / ".lock", "a")
        fcntl.flock(self.handle, fcntl.LOCK_EX)
        return self

    def __exit__(self, *_):
        self.handle.close()


def publish_feed(github, signer, product, channel, record, work, now=None):
    """Write the next feed for ``channel`` and upload it with its signature. Returns the feed."""
    name = "%s-%s.json" % (product, channel)
    with _FeedLock(signer.state_dir):
        github.ensure_channels_release()
        previous = current_feed(github, signer, product, channel)
        feed = feed_document(product, channel, next_sequence(previous, record), record, now or utc_now())
        data = encode_json(feed)
        path, sig_path = Path(work) / name, Path(work) / (name + ".sig")
        path.write_bytes(data)
        sig_path.write_bytes(signer.sign(data))
        verify.verify_feed_bytes(name, path.read_bytes(), sig_path.read_bytes(),
                                 min_sequence=previous["sequence"] + 1 if previous else 1)
        github.upload(CHANNELS_TAG, [path, sig_path])
        if github.dry_run:
            return feed
        write_journal(signer, name, path.read_bytes(), sig_path.read_bytes())
        published = read_feed(github, product, channel)
        require(published == feed, "read-back of %s differs from what was uploaded" % name)
    return feed


def engine_releases(github):
    """Published (non-draft) engine build releases with verified records, newest first."""
    result = []
    for release in github.releases():
        if release.get("draft") or not re.fullmatch(r"engine-" + verify.VERSION.pattern, release["tag_name"]):
            continue
        record = verified_release_record(github, release)
        if record is not None:
            result.append((release, record))
    result.sort(key=lambda item: version_key(item[1]["version"]), reverse=True)
    return result


VERSION_MENTION = re.compile(r"(?<![0-9.])([0-9]{4}\.[0-9]{2}\.[0-9]{2}\.[1-9][0-9]*)(?![0-9])")


def blocking_regressions(github, version):
    """Open ``regression`` issues filed against ``version`` or an earlier version.

    An issue names the versions it affects anywhere in its title or body. One that names
    no version blocks every promotion; one that only names newer versions does not block
    this one. Issue text is only regex-scanned, never executed or interpreted further.
    """
    blocking = []
    for issue in github.open_issues("regression"):
        text = (issue.get("title") or "") + "\n" + (issue.get("body") or "")
        named = [match for match in VERSION_MENTION.findall(text)]
        if not named or min(version_key(item) for item in named) <= version_key(version):
            blocking.append(issue["number"])
    return blocking


def clean_tree(path):
    """Remove a work tree, including the read-only (sealed) package prepare_diagnostic_engine leaves."""
    path = Path(path)
    if not path.exists():
        return
    for root, dirs, _ in os.walk(path):
        for name in dirs:
            target = Path(root) / name
            if not target.is_symlink():
                target.chmod(0o755)
    path.chmod(0o755)
    shutil.rmtree(path)
