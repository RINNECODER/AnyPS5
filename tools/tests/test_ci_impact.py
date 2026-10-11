from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ci"))
import impact

CPU, NATIVE, SHADER, REPLAY = "cpu-tcg", "native-runner", "shader-bridge", "guest-replay"
EVERY = {CPU, NATIVE, SHADER, REPLAY}


class ImpactSelectionTests(unittest.TestCase):
    def test_path_classes(self):
        cases = {
            "core/cpu/src/Main.cpp": {CPU, NATIVE},
            "core/cpu/fixtures/Homebrew.c": {CPU, NATIVE},
            "core/cpu/src/GuestMemoryMetal.cpp": {NATIVE, REPLAY},
            "core/cpu/videoout/VideoOutCaller.c": {NATIVE, REPLAY},
            "core/libs/prx/libSceVideoOut/Foo.cpp": {NATIVE, REPLAY},
            "core/libs/prx/libSceAgc/Foo.cpp": {NATIVE, REPLAY},
            "core/libs/prx/libSceAgcDriver/Execution/Metal/Foo.mm": {NATIVE, REPLAY},
            "core/libs/prx/libSceAgcDriver/Graphics/Metal/Foo.mm": {NATIVE, SHADER, REPLAY},
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
            "3rdparty/unicorn": EVERY,
            ".github/workflows/macos-metal.yml": EVERY,
            "tools/ci/impact.py": EVERY,
            "tools/package_release.py": EVERY,
            "tools/tests/test_native_diagnostic_package.py": EVERY,
            "core/libs/prx/libkernel/Foo.cpp": EVERY,
            "totally/new/place.txt": EVERY,
        }
        for path, expected in cases.items():
            with self.subTest(path=path):
                self.assertEqual(set(impact.jobs_for_path(path)), expected)

    def test_union_over_files(self):
        got = impact.select(["docs/a.md", "core/cpu/src/Main.cpp", "core/shader/x.cpp"])
        self.assertEqual(got, EVERY)

    def test_docs_only_selects_nothing(self):
        self.assertEqual(impact.select(["docs/a.md", "README.md"]), set())

    def test_empty_diff_selects_nothing(self):
        self.assertEqual(impact.select([]), set())

    def test_upstream_sync_label_selects_all(self):
        self.assertEqual(impact.select(["docs/a.md"], ["upstream-sync"]), EVERY)

    def test_unknown_path_among_docs_selects_all(self):
        self.assertEqual(impact.select(["docs/a.md", "mystery.bin"]), EVERY)


if __name__ == "__main__":
    unittest.main()
