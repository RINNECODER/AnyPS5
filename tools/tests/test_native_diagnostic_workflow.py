"""Contracts for the native fixture attachment and the owned build job limit.

The authoring gate: runtime sources request inner Ninja -j8, so merely bounding
outer jobs still exceeds the authorized limit. The real generated launcher must
remove that request without losing target arguments. Existing workflow tests do
not execute an inner build launcher. CMake's configure wrapper must attach the
finite fragments and the upstream correctness subtree exactly once after production targets exist, including after return(), and
fail before generation if the native production target is absent. Existing
CTest XML checks cannot catch a configure-order or activation regression.
Both controls use the production entry points without a test-only source seam.
"""
from contextlib import nullcontext
import importlib.util
import json
import os
from pathlib import Path
from types import SimpleNamespace
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
    def test_main_upstream_registration_is_required_by_native_qualification(self):
        """Real reachable registrations, not a copied list, must be declared.

        Contract: the native manifest must include reachable upstream controls.
        Omitting the five pixel-sampler cases makes a built candidate fail the
        exact package gate. Synthetic attachment/inventory controls miss new
        leaf registrations. This configures the actual upstream subtree with its prerequisite targets;
        no engine build, dependency cache or fixture execution is needed.
        """
        source = self.root / 'registration-source'
        source.mkdir()
        upstream = TOOLS.parent / 'core/shader/recompiler/MetalReplay/UpstreamCorrectness'
        (source / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.24)\n'
            'project(RegistrationContract LANGUAGES CXX OBJCXX)\n'
            'enable_testing()\nset(BUILD_TESTING ON)\n'
            'function(add_test_executable target)\n'
            'add_executable(${target} ${ARGN})\nendfunction()\n'
            'add_library(anyps5_metal_native_execution INTERFACE)\n'
            'add_library(anyps5_metal_guest_recompiler INTERFACE)\n'
            'add_library(anyps5_metal_utilities INTERFACE)\n'
            'set_target_properties(anyps5_metal_utilities PROPERTIES ANYPS5_METALLIB "fixture.metallib")\n'
            'add_subdirectory("' + upstream.as_posix() + '" upstream-correctness)\n')
        build = self.root / 'registration-build'
        configured = subprocess.run(['cmake', '-S', source, '-B', build],
                                    capture_output=True, text=True, timeout=60)
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        inventory = json.loads(subprocess.check_output(
            ['ctest', '--test-dir', build, '--show-only=json-v1'], text=True, timeout=15))
        names = [test['name'] for test in inventory['tests']]
        self.assertTrue(names, 'real upstream graph registered no controls')
        self.assertEqual(sum(name.startswith('anyps5_metal_pixel_sampler_') for name in names), 5,
                         'production pixel-sampler registration group was not activated')
        self.assertEqual(len(names), len(set(names)), 'duplicate upstream registration')
        self.assertFalse(set(names) - workflow.required_tests('native'),
                         'registered main controls absent from native qualification: ' +
                         repr(sorted(set(names) - workflow.required_tests('native'))))

    def test_pre_run_inventory_rejects_missing_extra_and_duplicate_before_execution(self):
        """Pre-run JSON must fail closed independently of post-run XML checks."""
        expected = {'cpu_contract', 'native_fixture'}
        valid = [{'name': name} for name in sorted(expected)]
        args = SimpleNamespace(source=self.root, macps_source=self.root, output=self.root,
                               profile='native', revision='a' * 40,
                               macps_revision='b' * 40, dependency_cache=None)
        dependencies = {name: {} for name in workflow.DEPENDENCIES}
        cases = {'valid': valid, 'missing': valid[:1],
                 'extra': valid + [{'name': 'unexpected'}],
                 'duplicate': valid + [valid[0]]}
        for case, tests in cases.items():
            with self.subTest(case=case), tempfile.TemporaryDirectory(dir=self.root) as folder:
                args.output = Path(folder)
                executed = []

                def run(name, argv, **kwargs):
                    executed.append(name)
                    if name == 'ctest-inventory':
                        return json.dumps({'tests': tests})
                    if name == 'combined-native':
                        raise RuntimeError('control reached combined-suite boundary')
                    return ''

                with patch.object(workflow, 'required_tests', return_value=expected), \
                     patch.object(workflow, 'clone_exact'), \
                     patch.object(workflow, 'setup_dependencies', return_value=dependencies), \
                     patch.object(workflow, 'clean_revision', return_value={}), \
                     patch.object(workflow, 'bounded_ninja', return_value=TOOLS / 'prepare_diagnostic_engine.py'), \
                     patch.object(workflow, 'artifact_records', return_value={}), \
                     patch.object(workflow, 'digest', return_value='digest'), \
                     patch.object(workflow, 'Lease', side_effect=lambda *a, **kw: nullcontext()), \
                     patch.object(workflow.os, 'uname', return_value=SimpleNamespace(machine='arm64')), \
                     patch.object(workflow, 'prepare_package') as package:
                    diagnostic = ('control reached combined-suite boundary' if case == 'valid' else
                                  'CTest required inventory differs from declared native profile')
                    with self.assertRaisesRegex(RuntimeError, diagnostic):
                        workflow.workflow(args, run, {})
                    package.assert_not_called()
                self.assertEqual('combined-native' in executed, case == 'valid')

    @unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64',
                         'Native CMake configuration requires Apple Silicon macOS')
    def test_native_fragments_follow_production_targets_and_fail_closed(self):
        self.assertIsNotNone(shutil.which('cmake'), 'CMake required for this contract control')
        source = self.root / 'runtime source with spaces'
        source.mkdir()
        for fragment in ('videoout', 'flip'):
            folder = source / 'core/cpu' / fragment
            folder.mkdir(parents=True)
            (folder / 'CMakeLists.txt').write_text(
                'if(NOT TARGET anyps5_native_module_runner)\nmessage(FATAL_ERROR "too early")\nendif()\n'
                'add_test(NAME qualified_' + fragment + ' COMMAND "${CMAKE_COMMAND}" -E true)\n')
        upstream = source / 'core/shader/recompiler/MetalReplay/UpstreamCorrectness'
        upstream.mkdir(parents=True)
        (upstream / 'CMakeLists.txt').write_text(
            'foreach(required IN ITEMS anyps5_native_module_runner anyps5_metal_native_execution '
            'anyps5_metal_guest_recompiler anyps5_metal_utilities)\n'
            'if(NOT TARGET ${required})\n'
            'message(FATAL_ERROR "upstream attached before native targets")\nendif()\nendforeach()\n'
            'get_property(visited GLOBAL PROPERTY independent_upstream_attached)\n'
            'if(visited)\nmessage(FATAL_ERROR "upstream attached more than once")\nendif()\n'
            'set_property(GLOBAL PROPERTY independent_upstream_attached TRUE)\n'
            'add_test(NAME qualified_upstream_subtree COMMAND "${CMAKE_COMMAND}" -E true)\n')
        wrapper = self.root / 'native-controls-source'
        wrapper.mkdir()
        shutil.copyfile(TOOLS / 'native_diagnostic_controls.cmake', wrapper / 'CMakeLists.txt')
        native = 'add_library(anyps5_native_module_runner INTERFACE)\n'
        for case, runner in (('enabled', native), ('missing-runner', '')):
            (source / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.24)\nproject(FiniteControls LANGUAGES NONE)\n'
                'enable_testing()\nadd_library(anyps5_cpu INTERFACE)\nadd_library(anyps5_cpu_agc INTERFACE)\n'
                'add_library(anyps5_metal_native_execution INTERFACE)\n'
                'add_library(anyps5_metal_guest_recompiler INTERFACE)\n'
                'add_library(anyps5_metal_utilities INTERFACE)\n' + runner + 'return()\n')
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
                    names = [test['name'] for test in inventory['tests']]
                    self.assertCountEqual(names, ['qualified_videoout', 'qualified_flip', 'qualified_upstream_subtree'])
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('requires the compiled native runner', result.stdout + result.stderr)
                    self.assertFalse((build / 'CTestTestfile.cmake').exists())


if __name__ == '__main__':
    unittest.main()
