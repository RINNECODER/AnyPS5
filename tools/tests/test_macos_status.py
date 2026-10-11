import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import macos_status


class MacosStatusTests(unittest.TestCase):
    def test_native_tests_resolve_named_sets(self):
        source = Path(tempfile.mkdtemp()) / "p.py"
        source.write_text("BASE_TESTS = set('''x y'''.split())\nNATIVE_REQUIRED_TESTS = BASE_TESTS | set('''z'''.split())\n")
        self.assertEqual(macos_status.parse_native_tests(source), {"x", "y", "z"})

    def test_repo_native_tests_match_module(self):
        import prepare_diagnostic_engine as p
        self.assertEqual(macos_status.parse_native_tests(), set(p.NATIVE_REQUIRED_TESTS))

    def test_missing_feed_is_none_and_present_feed_parses(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "engine-alpha.json").write_text(json.dumps(
                {"release": {"version": "1.2", "built_at": "2026-01-02T03:04:05Z", "commit": "abcdef0123456"}}))
            self.assertEqual(macos_status.feed_summary("alpha", d)["date"], "2026-01-02")
            self.assertIsNone(macos_status.feed_summary("stable", d))

    def test_coverage_counts_name_or_nid(self):
        libs = {"groups": [{"name": "libSceX", "label": "X", "done_names": ["sceFoo"], "todo_names": ["sceBar", "sceBaz"]}]}
        tokens = {"sceFoo", macos_status.nid_names.compute_nid("sceBar")}
        data = macos_status.coverage(libs, tokens)
        self.assertEqual((data["done"], data["total"]), (2, 3))


if __name__ == "__main__":
    unittest.main()
