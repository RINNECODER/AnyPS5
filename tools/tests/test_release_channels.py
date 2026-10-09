"""Release-channel pipeline contracts (tools/release, docs/RELEASES.md).

Protected contracts: the independent Ed25519 verifier matches RFC 8032 and agrees with
the CryptoKit signer; a tampered or replayed feed is rejected (and that rejection comes
from signature verification, proven by disabling it); feed sequences only move forward;
stable promotion needs RINNECODER's release:approved label event; package zips round-trip
safely; the smoke stage ladder reads the engine's diagnostics events.

GitHub is replaced by an in-memory release/issue store. It only stores and returns bytes
and records writes; every decision under test is made by the production code.
"""
import base64
import datetime as dt
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "release"))

import build_package  # noqa: E402
import promote  # noqa: E402
import releaselib  # noqa: E402
import smoke  # noqa: E402
import verify  # noqa: E402

NOW = dt.datetime(2026, 10, 12, 12, 0, tzinfo=dt.timezone.utc)
COMMIT = "a" * 40


class FakeGitHub:
    """In-memory releases (tag -> {name: bytes}), issues and recorded writes."""

    def __init__(self):
        self.dry_run = False
        self.assets, self.writes, self.comments, self.closed = {}, [], [], []
        self.issues, self.events = {}, {}

    def _release(self, tag, prerelease=True):
        return {"tag_name": tag, "draft": False, "prerelease": prerelease,
                "assets": [{"name": name, "id": index} for index, name in enumerate(sorted(self.assets[tag]))]}

    def release(self, tag):
        return self._release(tag) if tag in self.assets else None

    def releases(self):
        return [self._release(tag) for tag in self.assets]

    def asset_bytes(self, release, name):
        return self.assets.get(release["tag_name"], {}).get(name)

    def ensure_channels_release(self):
        self.assets.setdefault(releaselib.CHANNELS_TAG, {})

    def ensure_labels(self):
        pass

    def upload(self, tag, paths):
        for path in paths:
            self.assets[tag][Path(path).name] = Path(path).read_bytes()

    def write(self, description, args):
        self.writes.append(args)

    def open_issues(self, label):
        return [issue for issue in self.issues.values()
                if issue["state"] == "open" and label in {item["name"] for item in issue["labels"]}]

    def issue(self, number):
        return self.issues[number]

    def issue_events(self, number):
        return self.events.get(number, [])

    def comment(self, number, body):
        self.comments.append((number, body))

    def close_issue(self, number, reason="completed"):
        self.closed.append(number)

    def required_checks_passed(self, commit):
        return []


class SigningFixture(unittest.TestCase):
    """A throwaway Ed25519 key that the code under test treats as the release key."""

    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory(prefix="release-test-")
        cls.key = Path(cls.scratch.name) / "test.key"
        cls.key.write_text(base64.b64encode(os.urandom(32)).decode() + "\n")
        cls.key.chmod(0o600)
        cls.public = releaselib.Signer(cls.key).public_key()

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def setUp(self):
        patcher = patch.object(verify, "PUBLIC_KEY_B64", self.public)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.work = Path(tempfile.mkdtemp(dir=self.scratch.name))
        self.signer = releaselib.Signer(self.key, expected_public_key=self.public, state_dir=self.work / "state")

    def record(self, version, commit=COMMIT, built_at=NOW - dt.timedelta(days=2)):
        archive = self.work / verify.asset_name("engine", version)
        archive.write_bytes(b"zip bytes " + version.encode())
        return releaselib.release_record(version, commit, built_at, archive, "b" * 64, "notes")

    def publish_build(self, github, record):
        data = releaselib.encode_json(record)
        github.assets["engine-" + record["version"]] = {"release.json": data, "release.json.sig": self.signer.sign(data)}

    def signed_feed(self, sequence=7, version="2026.10.09.1"):
        data = releaselib.encode_json(releaselib.feed_document("engine", "alpha", sequence, self.record(version), NOW))
        return data, self.signer.sign(data)


class SignatureTests(SigningFixture):
    def test_rfc8032_vectors_verify_and_altered_inputs_do_not(self):
        vectors = [
            ("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
             "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
            ("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
             "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
        ]
        for public, message, signature in vectors:
            public, message, signature = bytes.fromhex(public), bytes.fromhex(message), bytes.fromhex(signature)
            self.assertTrue(verify.ed25519_verify(public, message, signature))
            self.assertFalse(verify.ed25519_verify(public, message + b"x", signature))
            self.assertFalse(verify.ed25519_verify(public, message, signature[:63] + bytes([signature[63] ^ 1])))

    def test_cryptokit_signature_verifies_with_the_independent_verifier(self):
        data = b'{"exact": "bytes"}\n'
        signature = self.signer.sign(data)
        self.assertTrue(signature.endswith(b"\n") and len(base64.b64decode(signature)) == 64)
        verify.verify_signed(data, signature)

    def test_signer_refuses_unsafe_key_file_or_wrong_key(self):
        self.key.chmod(0o644)
        try:
            with self.assertRaisesRegex(releaselib.ReleaseError, "0600"):
                self.signer.sign(b"x")
        finally:
            self.key.chmod(0o600)
        wrong = releaselib.Signer(self.key, expected_public_key=base64.b64encode(b"\1" * 32).decode())
        with self.assertRaisesRegex(releaselib.ReleaseError, "does not match"):
            wrong.sign(b"x")


class TamperTests(SigningFixture):
    def test_tampered_feed_rejected(self):
        data, signature = self.signed_feed(sequence=7)
        tampered = data.replace(b'"sequence": 7', b'"sequence": 8')
        self.assertNotEqual(tampered, data)
        with self.assertRaises(verify.VerificationError):
            verify.verify_feed_bytes("engine-alpha.json", tampered, signature)

    def test_tamper_test_fails_when_signature_verification_is_disabled(self):
        result = unittest.TestResult()
        with patch.object(verify, "ed25519_verify", lambda *_: True):
            TamperTests("test_tampered_feed_rejected").run(result)
        self.assertEqual(len(result.failures), 1, "the tamper test must depend on signature verification")
        self.assertEqual(result.errors, [])

    def test_signature_of_another_feed_and_replayed_sequence_rejected(self):
        data, signature = self.signed_feed(sequence=7)
        other_data, other_signature = self.signed_feed(sequence=8)
        with self.assertRaises(verify.VerificationError):
            verify.verify_feed_bytes("engine-alpha.json", data, other_signature)
        with self.assertRaisesRegex(verify.VerificationError, "lower than the last accepted"):
            verify.verify_feed_bytes("engine-alpha.json", data, signature, min_sequence=8)
        with self.assertRaisesRegex(verify.VerificationError, "channel does not match"):
            verify.verify_feed_bytes("engine-beta.json", data, signature)
        self.assertEqual(verify.verify_feed_bytes("engine-alpha.json", other_data, other_signature, 8)["sequence"], 8)


class SequenceTests(SigningFixture):
    def test_feed_sequence_strictly_increases_and_never_moves_backwards(self):
        github = FakeGitHub()
        sequences = [releaselib.publish_feed(github, self.signer, "engine", "alpha", self.record(version), self.work,
                                             NOW)["sequence"] for version in ("2026.10.09.1", "2026.10.09.2", "2026.10.10.1")]
        self.assertEqual(sequences, [1, 2, 3])
        for stale in ("2026.10.10.1", "2026.10.09.3"):
            with self.assertRaisesRegex(releaselib.ReleaseError, "not newer"):
                releaselib.publish_feed(github, self.signer, "engine", "alpha", self.record(stale), self.work, NOW)
        self.assertEqual(releaselib.read_feed(github, "engine", "alpha")["sequence"], 3)

    def test_half_replaced_public_feed_recovers_from_journal_but_never_without_it(self):
        github = FakeGitHub()
        for version in ("2026.10.09.1", "2026.10.09.2"):
            releaselib.publish_feed(github, self.signer, "engine", "alpha", self.record(version), self.work, NOW)
        channels = github.assets[releaselib.CHANNELS_TAG]
        first = releaselib.encode_json(releaselib.feed_document("engine", "alpha", 1, self.record("2026.10.09.1"), NOW))
        channels["engine-alpha.json"] = first  # json replaced, .sig still the newer one: an interrupted upload
        stranger = releaselib.Signer(self.key, expected_public_key=self.public, state_dir=self.work / "elsewhere")
        with self.assertRaises(verify.VerificationError):
            releaselib.publish_feed(github, stranger, "engine", "alpha", self.record("2026.10.09.3"), self.work, NOW)
        feed = releaselib.publish_feed(github, self.signer, "engine", "alpha", self.record("2026.10.09.3"), self.work, NOW)
        self.assertEqual(feed["sequence"], 3)
        self.assertEqual(releaselib.read_feed(github, "engine", "alpha"), feed)

    def test_replayed_old_public_feed_cannot_reset_the_sequence(self):
        github = FakeGitHub()
        releaselib.publish_feed(github, self.signer, "engine", "alpha", self.record("2026.10.09.1"), self.work, NOW)
        old_pair = dict(github.assets[releaselib.CHANNELS_TAG])
        for version in ("2026.10.09.2", "2026.10.09.3"):
            releaselib.publish_feed(github, self.signer, "engine", "alpha", self.record(version), self.work, NOW)
        github.assets[releaselib.CHANNELS_TAG] = old_pair  # authentic sequence-1 pair put back
        with self.assertRaisesRegex(releaselib.ReleaseError, "not newer"):
            releaselib.publish_feed(github, self.signer, "engine", "alpha", self.record("2026.10.09.2"), self.work, NOW)
        feed = releaselib.publish_feed(github, self.signer, "engine", "alpha", self.record("2026.10.09.4"), self.work, NOW)
        self.assertEqual(feed["sequence"], 4)

    def test_version_counts_up_per_utc_day(self):
        tags = ["engine-2026.10.09.1", "engine-2026.10.09.2", "engine-2026.10.08.7", "channels"]
        self.assertEqual(releaselib.next_version(tags, NOW.replace(day=9)), "2026.10.09.3")
        self.assertEqual(releaselib.next_version(tags, NOW), "2026.10.12.1")


class StablePromotionTests(SigningFixture):
    def setUp(self):
        super().setUp()
        self.github = FakeGitHub()
        self.version = "2026.10.09.1"
        record = self.record(self.version)
        self.publish_build(self.github, record)
        releaselib.publish_feed(self.github, self.signer, "engine", "beta", record, self.work, NOW)
        self.github.issues[5] = {
            "number": 5, "state": "open", "user": {"login": "github-actions[bot]"},
            "title": promote.candidate_title(self.version),
            "body": "<!-- release-candidate: engine %s -->\nPromote." % self.version,
            "labels": [{"name": "release:stable-candidate"}, {"name": "release:approved"}]}

    def run_stable(self, approvers, issue=5):
        self.github.events[5] = [{"event": "labeled", "label": {"name": "release:approved"}, "actor": {"login": login}}
                                 for login in approvers]
        args = type("Args", (), {"issue": issue, "signer": self.signer})()
        return promote.stable(self.github, NOW, args)

    def test_promotes_only_when_rinnecoder_added_the_approval(self):
        self.assertEqual(self.run_stable(["RINNECODER"]), 0)
        stable = releaselib.read_feed(self.github, "engine", "stable")
        self.assertEqual((stable["sequence"], stable["release"]["version"]), (1, self.version))
        self.assertIn(["release", "edit", "engine-" + self.version, "-R", releaselib.REPO, "--prerelease=false", "--latest"],
                      self.github.writes)
        self.assertEqual(self.github.closed, [5])

    def test_refuses_approval_whose_latest_label_event_is_not_rinnecoder(self):
        for approvers in (["some-agent"], ["RINNECODER", "some-agent"], []):
            self.assertEqual(self.run_stable(approvers), 0)
            self.assertIsNone(releaselib.read_feed(self.github, "engine", "stable"), approvers)
        self.assertEqual(self.github.closed, [])
        self.assertTrue(all("not performed" in body for _, body in self.github.comments))

    def test_refuses_a_version_that_is_not_the_current_beta(self):
        newer = self.record("2026.10.09.3")
        self.publish_build(self.github, newer)
        releaselib.publish_feed(self.github, self.signer, "engine", "beta", newer, self.work, NOW)
        self.assertEqual(self.run_stable(["RINNECODER"]), 0)
        self.assertIsNone(releaselib.read_feed(self.github, "engine", "stable"))
        self.assertIn("not the current beta", self.github.comments[-1][1])

    def test_open_regression_blocks_stable(self):
        self.github.issues[9] = {"number": 9, "state": "open", "labels": [{"name": "regression"}],
                                 "title": "Crash on boot", "body": "Seen in 2026.10.08.4", "user": {"login": "x"}}
        self.assertEqual(self.run_stable(["RINNECODER"]), 0)
        self.assertIsNone(releaselib.read_feed(self.github, "engine", "stable"))

    def test_regression_naming_only_newer_versions_does_not_block(self):
        self.github.issues[9] = {"number": 9, "state": "open", "labels": [{"name": "regression"}],
                                 "title": "Regressed in 2026.10.10.2", "body": "", "user": {"login": "x"}}
        self.assertEqual(releaselib.blocking_regressions(self.github, self.version), [])
        self.assertEqual(releaselib.blocking_regressions(self.github, "2026.10.11.1"), [9])


class PackageArchiveTests(unittest.TestCase):
    def test_zip_round_trip_keeps_modes_and_bytes_at_the_root(self):
        with tempfile.TemporaryDirectory() as scratch:
            package = Path(scratch) / "package"
            (package / "bin").mkdir(parents=True)
            (package / "manifest.json").write_text("{}\n")
            (package / "bin" / "anyps5_cpu_run").write_bytes(b"\xcf\xfa\xed\xfe")
            (package / "bin" / "anyps5_cpu_run").chmod(0o555)
            (package / "manifest.json").chmod(0o444)
            archive = build_package.write_zip(package, Path(scratch) / "a.zip")
            again = build_package.write_zip(package, Path(scratch) / "b.zip")
            self.assertEqual(archive.read_bytes(), again.read_bytes(), "zip must be deterministic")
            out = verify.safe_extract(archive, Path(scratch) / "out")
            self.assertEqual((out / "bin" / "anyps5_cpu_run").read_bytes(), b"\xcf\xfa\xed\xfe")
            self.assertEqual(stat.S_IMODE((out / "bin" / "anyps5_cpu_run").stat().st_mode), 0o755)
            self.assertEqual(stat.S_IMODE((out / "manifest.json").stat().st_mode), 0o644)

    def test_safe_extract_rejects_escaping_and_symlink_entries(self):
        for name, mode in (("../evil", 0o100644), ("bin/link", 0o120777), ("/abs", 0o100644)):
            with tempfile.TemporaryDirectory() as scratch:
                buffer = io.BytesIO()
                with zipfile.ZipFile(buffer, "w") as bundle:
                    info = zipfile.ZipInfo(name)
                    info.external_attr = mode << 16
                    bundle.writestr(info, b"x")
                archive = Path(scratch) / "x.zip"
                archive.write_bytes(buffer.getvalue())
                with self.assertRaises(verify.VerificationError, msg=name):
                    verify.safe_extract(archive, Path(scratch) / "out")

    def test_privacy_scan_finds_local_paths_in_binaries(self):
        with tempfile.TemporaryDirectory() as scratch:
            (Path(scratch) / "lib").mkdir()
            (Path(scratch) / "lib" / "a.dylib").write_bytes(b"\0/Users/someone/src/x.cpp\0")
            (Path(scratch) / "clean").write_bytes(b"/private/tmp/anyps5-release/x")
            self.assertEqual(build_package.privacy_scan(scratch, {b"/Users/someone"}), ["lib/a.dylib"])


class SmokeStageTests(unittest.TestCase):
    def test_stage_ladder_from_diagnostics_events(self):
        def event(kind, **fields):
            return json.dumps(dict(schema_version=1, event=kind, **fields))
        cases = [
            ("", 0),
            ("Segmentation fault\n", 0),
            (event("error", code="input_unavailable", message="m"), 1),
            (event("error", code="loader_failure", message="SCE module graph: missing DT_NEEDED provider x.prx"), 2),
            (event("error", code="unsupported_service", message="m"), 3),
            (event("startup", entry=1) + "\n" + event("error", code="execution_limit", message="m"), 4),
            (event("startup", entry=1) + "\n" + event("guest_exit", exit_code=0), 5),
        ]
        for text, stage in cases:
            self.assertEqual(smoke.classify(text)[0], stage, text)

    def test_redaction_removes_title_paths(self):
        title = Path(tempfile.gettempdir()) / "Games" / "TITLE"
        self.assertEqual(smoke.redact("cannot open " + str(title) + "/eboot.bin", title), "cannot open <title>/eboot.bin")


if __name__ == "__main__":
    unittest.main()
