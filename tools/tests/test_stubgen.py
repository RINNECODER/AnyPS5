from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_conventions
import stubgen


class StubgenTests(unittest.TestCase):
    def test_default_debt_link_resolves_from_docs_dev(self):
        _, td = stubgen.generate("libkernel", ["AC1FtjqMCL0"], {})
        found = check_conventions.links("docs/dev/TechnicalDebt.md", td.strip())
        self.assertTrue(found)
        for item in found:
            self.assertEqual(item[1], "core/libs/prx/libkernel/Unimplemented.cpp")

    def test_explicit_td_path_is_verbatim(self):
        _, td = stubgen.generate("libkernel", ["AC1FtjqMCL0"], {}, td_path="x/y.cpp")
        self.assertIn("(x/y.cpp)", td)

    def test_code_still_generated(self):
        code, _ = stubgen.generate("libkernel", ["AC1FtjqMCL0"], {})
        self.assertIn("APS5_VABI", code)
        self.assertIn("NotImplemented_nid_no_patch", code)

    def test_prx_suffix_is_stripped_from_module_name(self):
        nids = ["AAAAAAAAAAA", "BBBBBBBBBBB"]
        expected = stubgen.generate("libSceAmpr", nids, {})
        self.assertIn("sceAmprUnknown00", expected[0])
        self.assertIn("core/libs/prx/libSceAmpr/", expected[1])
        for spelling in ("libSceAmpr.prx", "libSceAmpr.PRX"):
            with self.subTest(spelling=spelling):
                self.assertEqual(stubgen.generate(spelling, nids, {}), expected)

    def test_named_stub_reports_its_function_name(self):
        code = stubgen.generate("libSceAmpr", ["tNn5WBkta60", "AAAAAAAAAAA"], {"tNn5WBkta60": "sceAmprCommandBufferNop"})[0]
        named = code.split("sceAmprCommandBufferNop(void) {\n", 1)[1].split("\n", 1)[0]
        self.assertEqual(named, "    NotImplemented_nid_no_patch(__func__);")
        self.assertIn('NotImplemented_nid_no_patch("AAAAAAAAAAA");', code)


if __name__ == "__main__":
    unittest.main()
