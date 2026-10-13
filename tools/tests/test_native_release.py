"""Native engine release pipeline contracts (tools/release/native_*.py, docs/design/native-releases.md).

Protected contracts: a native package holds only its declared install tree (nothing title-shaped,
nothing outside it, in the directory or in the zip); the build gate fails on a failing or missing
test; native records, tags and feeds are a separate product that cannot be read as an engine
release; beta needs 24 h on alpha, no open native-regression issue and a manifest that proves the
build gates; stable needs the explicit confirm, the current native-beta and the maintainer.

GitHub is the in-memory store from test_release_channels.py; every decision under test is made by
the production code. The package downloads are replaced by a small real package directory.
"""
import datetime as dt
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "release"))

import native_package  # noqa: E402
import native_promote  # noqa: E402
import releaselib  # noqa: E402
import verify  # noqa: E402
from test_release_channels import COMMIT, NOW, FakeGitHub, SigningFixture  # noqa: E402

GOOD_TREE = ["bin/native_aot", "compiler/compiler.py", "python/capstone/__init__.py", "lib/libcapstone.dylib",
             "runtime/lib/libanyps5_native_image.a", "runtime/include/NativeAotImage.hpp",
             "share/AnyPS5Utilities.metallib", "share/LICENSE"]


def write_tree(root, names):
    for name in names:
        path = Path(root) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"x " + name.encode())
    return list(names)


def ctest_text(names, status="Passed"):
    return "\n".join("%d/%d Test  #%d: %s ............   %s   0.01 sec" % (i + 1, len(names), i + 1, name, status)
                     for i, name in enumerate(names))


class InstallTreeGuardTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="native-guard-")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name) / "package"
        self.declared = write_tree(self.root, GOOD_TREE)

    def test_declared_tree_passes(self):
        native_package.check_install_tree(self.root, self.declared)

    def test_files_outside_the_declared_tree_fail(self):
        for extra in ("notes.txt", "titles/PPSA00000/eboot.bin", "extras/readme.md"):
            with self.subTest(extra=extra):
                write_tree(self.root, [extra])
                with self.assertRaisesRegex(releaselib.ReleaseError, "native package guard"):
                    native_package.check_install_tree(self.root, self.declared)
                (self.root / extra).unlink()

    def test_title_shaped_and_undeclared_files_fail_inside_allowed_roots(self):
        for extra in ("share/eboot.bin", "share/title.sprx", "share/param.sfo", "runtime/lib/module.o", "compiler/ledger.json",
                      "share/sce_sys/icon0.png", "compiler/undeclared.py"):
            with self.subTest(extra=extra):
                write_tree(self.root, [extra])
                with self.assertRaisesRegex(releaselib.ReleaseError, "native package guard"):
                    native_package.check_install_tree(self.root, self.declared)
                (self.root / extra).unlink()
                parent = (self.root / extra).parent
                if parent.name == "sce_sys":
                    parent.rmdir()

    def test_a_declared_file_that_is_missing_and_symlinks_fail(self):
        (self.root / "share/LICENSE").unlink()
        with self.assertRaisesRegex(releaselib.ReleaseError, "declared but is missing"):
            native_package.check_install_tree(self.root, self.declared)
        (self.root / "share/LICENSE").symlink_to(self.root / "bin/native_aot")
        with self.assertRaisesRegex(releaselib.ReleaseError, "symbolic link"):
            native_package.check_install_tree(self.root, self.declared)

    def test_zip_must_hold_exactly_the_package_tree(self):
        archive = Path(self.scratch.name) / "good.zip"
        with zipfile.ZipFile(archive, "w") as bundle:
            for name in self.declared:
                bundle.writestr(name, b"x")
        native_package.check_zip_tree(archive, self.root)
        with zipfile.ZipFile(archive, "a") as bundle:
            bundle.writestr("titles/eboot.bin", b"x")
        with self.assertRaisesRegex(releaselib.ReleaseError, "zip contents differ"):
            native_package.check_zip_tree(archive, self.root)


class TestGateTests(unittest.TestCase):
    NAMES = ["native_aot_unit_%d" % i for i in range(120)] + list(native_package.REQUIRED_TESTS)

    def test_all_passing_with_required_tests_is_accepted(self):
        native_package.check_ctest(native_package.parse_ctest(ctest_text(self.NAMES)))

    def test_a_failing_test_or_a_missing_required_test_is_rejected(self):
        results = native_package.parse_ctest(ctest_text(self.NAMES))
        self.assertEqual(len(results), len(self.NAMES))
        results["native_aot_unit_3"] = "Failed"
        with self.assertRaisesRegex(releaselib.ReleaseError, "did not pass"):
            native_package.check_ctest(results)
        without_cube = [name for name in self.NAMES if name != "native_aot_hello_notify"]
        with self.assertRaisesRegex(releaselib.ReleaseError, "required native tests"):
            native_package.check_ctest(native_package.parse_ctest(ctest_text(without_cube)))
        with self.assertRaisesRegex(releaselib.ReleaseError, "only"):
            native_package.check_ctest(native_package.parse_ctest(ctest_text(list(native_package.REQUIRED_TESTS))))

    def test_failure_lines_are_parsed_as_failures(self):
        text = "1/2 Test  #1: native_aot_a ......***Failed    0.10 sec\n2/2 Test  #2: native_aot_b .....   Passed   0.01 sec"
        self.assertEqual(native_package.parse_ctest(text), {"native_aot_a": "Failed", "native_aot_b": "Passed"})


class NativeProductTests(SigningFixture):
    def record(self, version, commit=COMMIT, built_at=NOW - dt.timedelta(days=2)):
        archive = self.work / verify.asset_name("native", version)
        archive.write_bytes(b"native zip " + version.encode())
        return releaselib.release_record(version, commit, built_at, archive, "b" * 64, "notes", product="native")

    def test_record_is_native_and_not_an_engine_record(self):
        record = self.record("2026.10.12.1")
        self.assertEqual((record["product"], record["kind"], record["arch"]), ("native", "native", "arm64"))
        self.assertEqual(record["asset"]["name"], "anyps5-native-2026.10.12.1-macos-arm64.zip")
        self.assertTrue(record["asset"]["url"].endswith("/native-2026.10.12.1/anyps5-native-2026.10.12.1-macos-arm64.zip"))
        with self.assertRaises(verify.VerificationError):
            verify.check_release(record, "engine")
        bad = dict(record, kind="engine")
        with self.assertRaisesRegex(verify.VerificationError, "kind native"):
            verify.check_release(bad, "native")

    def test_native_feed_cannot_be_published_as_an_engine_feed(self):
        data = releaselib.encode_json(releaselib.feed_document("native", "alpha", 1, self.record("2026.10.12.1"), NOW))
        signature = self.signer.sign(data)
        self.assertEqual(verify.verify_feed_bytes("native-alpha.json", data, signature)["product"], "native")
        with self.assertRaises(verify.VerificationError):
            verify.verify_feed_bytes("engine-alpha.json", data, signature)

    def test_version_numbering_counts_native_tags_only(self):
        tags = ["engine-2026.10.12.1", "engine-2026.10.12.2", "native-2026.10.12.1"]
        self.assertEqual(releaselib.next_version(tags, NOW, "native"), "2026.10.12.2")
        self.assertEqual(releaselib.next_version(tags, NOW, "engine"), "2026.10.12.3")
        self.assertEqual(releaselib.next_version(["engine-2026.10.12.5"], NOW, "native"), "2026.10.12.1")


class PromotionTests(SigningFixture):
    def setUp(self):
        super().setUp()
        self.github = FakeGitHub()
        self.packages = {}
        self.args = type("Args", (), {"signer": self.signer, "version": "", "skip_beta_age": False, "confirm": False})()

    def make_package(self, record, **overrides):
        """A small real native package whose manifest records the build gates (overrides replace them)."""
        root = Path(tempfile.mkdtemp(dir=self.work)) / "package"
        write_tree(root, GOOD_TREE)
        files = {name: {"sha256": verify.sha256_file(root / name), "size": (root / name).stat().st_size} for name in GOOD_TREE}
        validation = {"host": {"machine": "arm64", "proc_translated": 0},
                      "ctest": {"passed": 324, "failed": 0, "required": list(native_package.REQUIRED_TESTS)},
                      "native_gpu_cube_check": {"result": "PASS", "frames": 8, "different_pixels": 0}, "hello_notify": "PASS"}
        manifest = {"schema": 1, "kind": "native", "arch": "arm64", "contains_title_data": False,
                    "native_version": record["version"], "source_revisions": {"AnyPS5": {"commit": record["commit"]}},
                    "validation": dict(validation, **overrides), "files": files}
        (root / "manifest.json").write_bytes(releaselib.encode_json(manifest))
        return root

    def publish(self, version, age_hours, **overrides):
        record = self.record(version, built_at=NOW - dt.timedelta(hours=age_hours))
        data = releaselib.encode_json(record)
        self.github.assets["native-" + version] = {"release.json": data, "release.json.sig": self.signer.sign(data)}
        self.packages[version] = self.make_package(record, **overrides)
        return record

    record = NativeProductTests.record

    def run_action(self, action, **flags):
        actor = flags.pop("actor", "RINNECODER")
        for key, value in flags.items():
            setattr(self.args, key, value)
        fetch = lambda record, scratch, label: self.packages[record["version"]]  # noqa: E731
        with patch.object(native_promote.promote, "fetch_package", fetch), \
                patch.object(native_package, "smoke", lambda *a, **k: "PASS"), \
                patch.dict("os.environ", {"GITHUB_ACTOR": actor}):
            try:
                return getattr(native_promote, action)(self.github, NOW, self.args)
            except releaselib.ReleaseError:
                return 1  # what native_promote.main() does with a failed verification

    def feed(self, channel):
        assets = self.github.assets.get(releaselib.CHANNELS_TAG, {})
        name = "native-%s.json" % channel
        return verify.verify_feed_bytes(name, assets[name], assets[name + ".sig"]) if name in assets else None

    def test_beta_holds_until_24_hours_then_publishes_a_native_beta_feed_only(self):
        self.publish("2026.10.12.1", age_hours=2)
        self.assertEqual(self.run_action("beta"), 0)
        self.assertIsNone(self.feed("beta"))
        self.publish("2026.10.11.1", age_hours=30)
        self.assertEqual(self.run_action("beta"), 0)
        feed = self.feed("beta")
        self.assertEqual((feed["product"], feed["channel"], feed["sequence"], feed["release"]["version"]),
                         ("native", "beta", 1, "2026.10.11.1"))
        self.assertEqual(sorted(name for name in self.github.assets[releaselib.CHANNELS_TAG]), ["native-beta.json", "native-beta.json.sig"])

    def test_skip_beta_age_promotes_the_young_build_but_other_gates_still_apply(self):
        self.publish("2026.10.12.1", age_hours=2, hello_notify="FAIL")
        self.assertEqual(self.run_action("beta", skip_beta_age=True), 1)
        self.assertIsNone(self.feed("beta"))
        self.publish("2026.10.12.2", age_hours=2)
        self.assertEqual(self.run_action("beta", skip_beta_age=True, version="2026.10.12.2"), 0)
        self.assertEqual(self.feed("beta")["release"]["version"], "2026.10.12.2")

    def test_beta_gate_failures_in_the_manifest_block_promotion(self):
        for override in ({"host": {"machine": "arm64", "proc_translated": 1}},
                         {"native_gpu_cube_check": {"result": "PASS", "frames": 8, "different_pixels": 3}},
                         {"ctest": {"passed": 324, "failed": 2, "required": list(native_package.REQUIRED_TESTS)}}):
            with self.subTest(override=list(override)):
                self.packages = {}
                self.github.assets.clear()
                self.publish("2026.10.11.1", age_hours=30, **override)
                self.assertEqual(self.run_action("beta"), 1)
                self.assertIsNone(self.feed("beta"))

    def test_open_native_regression_issue_holds_beta(self):
        self.publish("2026.10.11.1", age_hours=30)
        self.github.issues[2] = {"number": 2, "state": "open", "labels": [{"name": "native-regression"}], "title": "native broke",
                                 "body": "", "user": {"login": "x"}}
        self.assertEqual(self.run_action("beta"), 0)
        self.assertIsNone(self.feed("beta"))

    def test_an_engine_regression_issue_does_not_hold_native_beta(self):
        self.publish("2026.10.11.1", age_hours=30)
        self.github.issues[1] = {"number": 1, "state": "open", "labels": [{"name": "regression"}], "title": "engine broke",
                                 "body": "", "user": {"login": "x"}}
        self.assertEqual(self.run_action("beta"), 0)
        self.assertIsNotNone(self.feed("beta"))

    def test_stable_needs_confirm_the_current_beta_and_the_maintainer(self):
        self.publish("2026.10.11.1", age_hours=60)
        self.publish("2026.10.12.1", age_hours=30)
        self.run_action("beta")  # the newest aged build becomes native-beta
        beta_version, other = "2026.10.12.1", "2026.10.11.1"
        self.assertEqual(self.feed("beta")["release"]["version"], beta_version)
        self.assertEqual(self.run_action("stable", version=beta_version, confirm=False), 0)
        self.assertIsNone(self.feed("stable"))
        self.assertEqual(self.run_action("stable", version=beta_version, confirm=True, actor="some-agent"), 0)
        self.assertIsNone(self.feed("stable"))
        self.assertEqual(self.run_action("stable", version=other, confirm=True), 0)
        self.assertIsNone(self.feed("stable"))
        self.assertEqual(self.run_action("stable", version=beta_version, confirm=True), 0)
        stable = self.feed("stable")
        self.assertEqual((stable["product"], stable["channel"], stable["release"]["version"]), ("native", "stable", beta_version))
        edits = [write for write in self.github.writes if write[:2] == ["release", "edit"]]
        self.assertEqual(len(edits), 1)
        self.assertIn("--latest=false", edits[0])
        self.assertNotIn("--latest", edits[0])


if __name__ == "__main__":
    unittest.main()
