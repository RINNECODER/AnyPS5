"""Contracts for the native fixture attachment and the owned build job limit.

The authoring gate: runtime sources request inner Ninja -j8, so merely bounding
outer jobs still exceeds the authorized limit. The real generated launcher must
remove that request without losing target arguments. Existing workflow tests do
not execute an inner build launcher. CMake's configure wrapper must attach the
finite fragments after production targets exist, including after return(), and
fail before generation if the native production target is absent. Existing
CTest XML checks cannot catch a configure-order or activation regression.
Both controls use the production entry points without a test-only source seam.
"""
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location('native_workflow', TOOLS / 'prepare_diagnostic_engine.py')
workflow = importlib.util.module_from_spec(spec)
spec.loader.exec_module(workflow)


class NativeWorkflowContracts(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='native-workflow-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def test_owned_inner_ninja_caps_parallelism_and_preserves_targets(self):
        fake = self.root / 'ninja'
        fake.write_text('#!' + sys.executable + '\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n')
        fake.chmod(0o755)
        with patch.dict(os.environ, PATH=str(self.root) + os.pathsep + os.environ['PATH']):
            launcher = workflow.bounded_ninja(self.root)
        for jobs in (['-j', '8'], ['-j8'], ['--jobs=8'], ['--jobs', '8'], ['-j0']):
            with self.subTest(jobs=jobs):
                value = json.loads(subprocess.check_output(
                    [launcher, '-C', 'TCG build with spaces', *jobs, 'libqemu-x86_64-softmmu.dylib'], text=True))
                self.assertEqual(value, ['-j', '2', '-C', 'TCG build with spaces', 'libqemu-x86_64-softmmu.dylib'])
        query = json.loads(subprocess.check_output([launcher, '--version'], text=True))
        self.assertEqual(query, ['-j', '2', '--version'])

    @unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64',
                         'Native CMake configuration requires Apple Silicon macOS')
    def test_native_fragments_follow_production_targets_and_fail_closed(self):
        self.assertIsNotNone(shutil.which('cmake'), 'CMake required for this contract control')
        source = self.root / 'source'
        source.mkdir()
        for fragment in ('videoout', 'flip'):
            folder = source / 'core/cpu' / fragment
            folder.mkdir(parents=True)
            (folder / 'CMakeLists.txt').write_text(
                'if(NOT TARGET anyps5_native_module_runner)\nmessage(FATAL_ERROR "too early")\nendif()\n'
                'add_test(NAME qualified_' + fragment + ' COMMAND "${CMAKE_COMMAND}" -E true)\n')
        wrapper = self.root / 'native-controls-source'
        wrapper.mkdir()
        shutil.copyfile(TOOLS / 'native_diagnostic_controls.cmake', wrapper / 'CMakeLists.txt')
        native = 'add_library(anyps5_native_module_runner INTERFACE)\n'
        for case, runner in (('enabled', native), ('missing-runner', '')):
            (source / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.24)\nproject(FiniteControls LANGUAGES NONE)\n'
                'enable_testing()\nadd_library(anyps5_cpu INTERFACE)\nadd_library(anyps5_cpu_agc INTERFACE)\n' + runner + 'return()\n')
            build = self.root / case
            result = subprocess.run(['cmake', '-S', wrapper, '-B', build, '-DBUILD_TESTING=ON',
                '-DANYPS5_CPU_NATIVE_MODULE_RUNNER=ON',
                '-DANYPS5_DIAGNOSTIC_SOURCE:PATH=' + str(source)],
                capture_output=True, text=True)
            with self.subTest(case=case):
                if runner:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    inventory = json.loads(subprocess.check_output(
                        ['ctest', '--test-dir', build, '--show-only=json-v1'], text=True))
                    self.assertEqual({t['name'] for t in inventory['tests']}, {'qualified_videoout', 'qualified_flip'})
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('requires the compiled native runner', result.stdout + result.stderr)
                    self.assertFalse((build / 'CTestTestfile.cmake').exists())


if __name__ == '__main__':
    unittest.main()
