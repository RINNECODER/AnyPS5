import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ci"))
import impact

SHADER, REPLAY = "shader-bridge", "guest-replay"
ROSETTA = "rosetta-libs"
EVERY = {SHADER, REPLAY, ROSETTA}


class ImpactSelectionTests(unittest.TestCase):
    def test_path_classes(self):
        cases = {
            "core/libs/prx/libSceVideoOut/Foo.cpp": {REPLAY},
            "core/libs/prx/libSceAgc/Foo.cpp": {REPLAY},
            "core/libs/prx/libSceAgcDriver/Execution/Metal/Foo.mm": {REPLAY},
            "core/libs/prx/libSceAgcDriver/Graphics/Metal/Foo.mm": {SHADER, REPLAY},
            "core/shader/recompiler/MetalBackend/MetalShaderBridge.cpp": {SHADER, REPLAY},
            "core/shader/recompiler/MetalReplay/Foo.mm": {REPLAY},
            "core/shader/other.cpp": {SHADER, REPLAY},
            "docs/dev/Foo.md": set(),
            "README.md": set(),
            "tools/stubgen.py": set(),
            "tools/tests/test_stubgen.py": set(),
            "CMakeLists.txt": EVERY,
            "cmake/SystemDeps.cmake": EVERY,
            ".gitmodules": EVERY,
            "3rdparty/SDL2": EVERY,
            ".github/workflows/macos-metal.yml": EVERY,
            "tools/ci/impact.py": EVERY,
            "core/libs/prx/libkernel/Foo.cpp": EVERY,
            "totally/new/place.txt": EVERY,
        }
        for path, expected in cases.items():
            with self.subTest(path=path):
                self.assertEqual(set(impact.jobs_for_path(path)), expected)

    def test_union_over_files(self):
        got = impact.select(["docs/a.md", "core/shader/x.cpp"])
        self.assertEqual(got, {SHADER, REPLAY})

    def test_docs_only_selects_nothing(self):
        self.assertEqual(impact.select(["docs/a.md", "README.md"]), set())

    def test_empty_diff_selects_nothing(self):
        self.assertEqual(impact.select([]), set())

    def test_upstream_sync_label_selects_all(self):
        self.assertEqual(impact.select(["docs/a.md"], ["upstream-sync"]), EVERY)

    def test_rosetta_libs_for_the_direct_path_and_global_changes(self):
        for path in ("core/libs/prx/libkernel/Foo.cpp", "core/libs/prx/libSceAgc/Foo.cpp",
                     "core/relinker/main.cpp", "cmake/RosettaLibs.cmake", "tools/rosetta/run_title.py",
                     "core/libs/prx/libSceAgcDriver/Execution/Metal/DirectDriver.mm",
                     "core/libs/prx/libSceVideoOut/src/DirectDisplayWindow.mm",
                     ".github/workflows/macos-metal.yml", "CMakeLists.txt", "tools/ci/impact.py"):
            with self.subTest(path=path):
                self.assertIn(ROSETTA, impact.select([path]))
        for path in ("core/shader/x.cpp", "docs/a.md", "tools/stubgen.py",
                     "tools/other.py"):
            with self.subTest(path=path):
                self.assertNotIn(ROSETTA, impact.select([path]))

    def test_push_selects_every_job_including_rosetta_libs(self):
        with tempfile.TemporaryDirectory() as scratch:
            output = Path(scratch) / "out"
            with contextlib.redirect_stdout(io.StringIO()):
                impact.main(["--all", "--github-output", str(output)])
            self.assertIn("rosetta-libs=true", output.read_text().splitlines())

    def test_unknown_path_among_docs_selects_all(self):
        self.assertEqual(impact.select(["docs/a.md", "mystery.bin"]), EVERY)


if __name__ == "__main__":
    unittest.main()
