"""Native package boundary controls using independently compiled protocol fixtures.

Contract/regression/coverage-gate records live in tools05/tests-evidence.json.
These are ARM64 Mach-O packaging fixtures, not the production CPU runner, guest
execution, or production native acceptance. Run compile/execution under the
caller's canonical tcg-build lease. No production-only seams are introduced.
"""
import json
import re
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import diagnostic_package as package


def execute(_name, argv, cwd=None, env=None, timeout=120):
    result = subprocess.run([str(v) for v in argv], cwd=cwd, env=env,
                            capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{argv[0]} exited {result.returncode}: {result.stderr}")
    return result.stdout


def git(path, *args):
    return execute('git', ['git', '-C', path, *args]).strip()


def repository(path):
    path.mkdir(parents=True)
    git(path, 'init', '-q')
    git(path, 'config', 'user.name', 'Independent package control')
    git(path, 'config', 'user.email', 'control@example.invalid')
    (path / 'tracked.txt').write_text('committed packaging fixture source\n')
    git(path, 'add', 'tracked.txt')
    git(path, 'commit', '-qm', 'fixture source')


class PackageFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='native-package-control-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source, self.gpu, self.build = (self.root / name for name in ('source', 'gpu', 'build'))
        repository(self.source)
        repository(self.gpu)
        self.revisions = {}
        repositories = {'AnyPS5': self.source, 'GPU': self.gpu}
        for key, relative in (('TCG', '3rdparty/anyps5-tcg'), ('Unicorn', '3rdparty/unicorn')):
            child = self.source / relative
            repository(child)
            commit = git(child, 'rev-parse', 'HEAD')
            git(self.source, 'update-index', '--add', '--cacheinfo', f'160000,{commit},{relative}')
            repositories[key] = child
        git(self.source, 'commit', '-qm', 'pin dependency gitlinks')
        for key, repo in repositories.items():
            self.revisions[key] = {'commit': git(repo, 'rev-parse', 'HEAD'),
                                   'tree': git(repo, 'rev-parse', 'HEAD^{tree}'), 'clean_observed': True}
        self.build.mkdir()
        self.wrapper = self.build.parent / 'native-controls-source'
        self.wrapper.mkdir()
        shutil.copyfile(Path(__file__).resolve().parents[1] / 'native_diagnostic_controls.cmake',
                        self.wrapper / 'CMakeLists.txt')
        self.cache_values = {
            'CMAKE_HOME_DIRECTORY': str(self.wrapper),
            'ANYPS5_DIAGNOSTIC_SOURCE': str(self.source), 'ANYPS5_DIAGNOSTIC_CPU_SOURCE_DIR': str(self.source / 'core/cpu'),
            'ANYPS5_DIAGNOSTIC_CPU_BINARY_DIR': str(self.build / 'source/core/cpu'),
            'ANYPS5_CPU_METAL_SOURCE_DIR': str(self.gpu), 'ANYPS5_CPU_BACKEND': 'TCG',
            'CMAKE_OSX_ARCHITECTURES': 'arm64', 'BUILD_TESTING': 'ON',
            'ANYPS5_CPU_NATIVE_MODULE_RUNNER': 'ON',
            'ANYPS5_CPU_RUNTIME_ONLY': 'ON', 'ANYPS5_CPU_SYSTEM_MODEL': 'ON',
            'ANYPS5_BUILD_METAL_SHADER_BRIDGE_TESTS': 'ON',
        }
        self.write_cache()
        self.destination = self.root / 'package with spaces'

    def write_cache(self):
        (self.build / 'CMakeCache.txt').write_text(''.join(f'{key}:STRING={value}\n'
                                                       for key, value in self.cache_values.items()))

    def prepare(self, **kwargs):
        return package.prepare_package(self.source, self.build, self.gpu, self.destination,
                                       self.revisions, execute, **kwargs)


class NativeProfileAdmission(PackageFixture):
    def test_stale_native_cache_is_rejected_before_artifact_scan(self):
        for setting, value in (('ANYPS5_CPU_NATIVE_MODULE_RUNNER', 'OFF'),
                               ('ANYPS5_CPU_NATIVE_MODULE_RUNNER', None),
                               ('BUILD_TESTING', 'OFF'),
                               ('ANYPS5_CPU_RUNTIME_ONLY', 'OFF'),
                               ('ANYPS5_BUILD_METAL_SHADER_BRIDGE_TESTS', 'OFF'),
                               ('CMAKE_HOME_DIRECTORY', str(Path('/unrelated/source'))),
                               ('ANYPS5_CPU_BACKEND', 'UNICORN'),
                               ('ANYPS5_DIAGNOSTIC_SOURCE', str(Path('/unrelated/source'))),
                               ('ANYPS5_DIAGNOSTIC_CPU_BINARY_DIR', str(self.build / 'stale'))):
            with self.subTest(setting=setting, value=value):
                original = self.cache_values[setting]
                if value is None:
                    del self.cache_values[setting]
                else:
                    self.cache_values[setting] = value
                self.write_cache()
                with self.assertRaises(RuntimeError) as caught:
                    self.prepare(profile='native')
                self.assertNotIn('Missing native package input', str(caught.exception),
                                 'stale cache reached native artifact scan')
                self.assertFalse(self.destination.exists())
                self.cache_values[setting] = original

    def test_default_legacy_rejects_native_build_profile(self):
        # Valid legacy source identity ensures rejection belongs to the profile
        # guard rather than the wrapper HOME check. No compiled input is needed.
        self.cache_values['CMAKE_HOME_DIRECTORY'] = str(self.source)
        self.write_cache()
        with self.assertRaisesRegex(RuntimeError, 'CMake native module runner profile mismatch'):
            self.prepare()
        self.assertFalse(self.destination.exists())


# This input implements the compiled CLI protocol, not the package adapter result.
# A real linker gives it a bridge->leaf dependency; _NSGetExecutablePath makes
# utility discovery independent of cwd and of source/build directory existence.
CLI_SOURCE = r'''
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
extern int bridge(void);
static const char caps[] = CAPS;
int main(int argc, char **argv) {
    if (bridge() != 47) return 90;
    if (argc == 2 && strcmp(argv[1], "--capabilities-json") == 0) {
#ifdef DIFFERENT_AFTER_RELOCATION
        if (!strstr(argv[0], "/build/")) { puts("{}"); return 0; }
#endif
        puts(caps); return 0;
    }
    uint32_t size = PATH_MAX; char executable[PATH_MAX], resolved[PATH_MAX];
    if (_NSGetExecutablePath(executable, &size) || !realpath(executable, resolved)) return 91;
    char *slash = strrchr(resolved, '/'); if (!slash) return 92; *slash = 0;
    slash = strrchr(resolved, '/'); if (!slash) return 93; *slash = 0;
    strcat(resolved, "/fixtures/AnyPS5Utilities.metallib");
    FILE *f = fopen(resolved, "rb"); if (!f) return 94;
    char bytes[64] = {0}; size_t n = fread(bytes, 1, sizeof(bytes), f); fclose(f);
    if (n != sizeof("independent utility fixture\n") - 1 || memcmp(bytes, "independent utility fixture\n", n)) return 95;
    puts("fixture utility and dyld closure resolved"); return 0;
}
'''
CAPABILITIES = {
    'schema_version': 1, 'host_architecture': 'arm64', 'guest_architecture': 'x86_64',
    'backend': 'Modern QEMU TCG x86-64 dynamic translation', 'cpu_profile': 'Haswell',
    'supported_instruction_families': ['AVX', 'AVX2', 'F16C', 'FMA'],
    'supported_formats': ['static_elf64_x86_64', 'sce_elf64_x86_64'],
    'supported_containers': ['plain_self'], 'runtime_abi': 'linux_sysv',
    'runtime_abis': ['linux_sysv', 'sce_sysv'], 'sce_module_argument': '--sce-module',
    'resource_root_argument': '--resource-root', 'ps5_game_runtime_ready': False,
    'native_module_runner': {
        'enabled': True, 'owned_memory': 'live staged CPU/Metal publication',
        'provider_selection': 'actual parsed consumer SHA-256, size, scope and ELF symbol',
        'utility_metallib': '../fixtures/AnyPS5Utilities.metallib relative to engine',
        'wall_limit_ms': 0, 'idle_limit_ms': 0,
        'constraints': 'unbounded game profile by default; 0 means unlimited; idle limit bounds one continuous idle stretch; qualified provider subset only; high CPU owned stack/TLS are GPU read-only under written-page ABI; offline NP/WebAPI providers only; no retail gameplay evidence',
    },
}


@unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64',
                     'Real ARM64 Mach-O controls require Apple Silicon macOS')
class NativeCompiledPackaging(PackageFixture):
    @classmethod
    def setUpClass(cls):
        cls.compiled = tempfile.TemporaryDirectory(prefix='native-package-mach-o-')
        cls.addClassCleanup(cls.compiled.cleanup)
        cls.products = Path(cls.compiled.name).resolve()
        leaf, bridge = cls.products / 'libleaf.dylib', cls.products / 'libbridge.dylib'
        cls.compile('int leaf(void) { return 47; }', leaf, '-dynamiclib', '-Wl,-install_name,' + str(leaf))
        cls.compile('extern int leaf(void); int bridge(void) { return leaf(); }', bridge,
                    '-dynamiclib', str(leaf), '-Wl,-install_name,' + str(bridge))
        for name, caps, flags in (
            ('native', CAPABILITIES, []),
            ('legacy', {k: v for k, v in CAPABILITIES.items() if k != 'native_module_runner'}, []),
            ('changed', CAPABILITIES, ['-DDIFFERENT_AFTER_RELOCATION']),
            ('unsafe', {**CAPABILITIES, 'ps5_game_runtime_ready': True}, []),
        ):
            # Two-stage JSON quoting yields a C string literal containing JSON.
            cls.compile(CLI_SOURCE, cls.products / name, str(bridge),
                        '-DCAPS=' + json.dumps(json.dumps(caps, separators=(',', ':'))), *flags)

    @classmethod
    def compile(cls, text, output, *args):
        source = output.with_suffix(output.suffix + '.c')
        source.write_text(text)
        execute('compile-independent-fixture', ['xcrun', 'clang', '-arch', 'arm64',
                '-mmacosx-version-min=14.0', '-Wl,-headerpad_max_install_names', source, '-o', output, *args])

    def setUp(self):
        super().setUp()
        self.cpu = self.build / 'source/core/cpu'
        # These are public CMake output locations, including the native runner's
        # actual AGC target location and explicitly included VideoOut/flip roots.
        roots = [self.cpu / 'anyps5_cpu_run', self.cpu / 'anyps5_system_probe']
        roots += [self.build / 'tests' / name for name in (
            'anyps5_cpu_tests', 'anyps5_cpu_context_tests', 'anyps5_sce_loader_tests',
            'anyps5_sce_modules_tests', 'anyps5_sce_tls_tests', 'anyps5_sce_main_lifecycle_tests',
            'anyps5_guest_memory_tests', 'anyps5_sce_memory_import_tests',
            'anyps5_sce_videoout_import_tests', 'anyps5_sce_native_videoout_tests',
            'anyps5_cpu_metal_test', 'anyps5_cpu_memory_metal_test', 'anyps5_metal_agc_host_exports',
            'anyps5_guest_thread_tests', 'anyps5_native_module_runner_test',
            'anyps5_metal_optional_sgpr_replay',
        )]
        roots += [self.build / 'upstream-correctness' / name for name in (
            'anyps5_metal_normalized_load_replay', 'anyps5_metal_scalar_termination_replay')]
        roots += [self.build / 'native-videoout' / name for name in (
            'anyps5_cpu_videoout_native_fixture', 'anyps5_cpu_videoout_packed_hdr_fixture',
            'anyps5_cpu_videoout_admission_fixture')]
        roots += [self.build / 'native-flip' / name for name in (
            'anyps5_cpu_flip_contract_fixture', 'anyps5_cpu_flip_native_fixture')]
        roots += [self.cpu / 'metal-source/core/shader/recompiler/MetalReplay/cpu-agc' /
                  f'anyps5_cpu_agc_{case}_fixture' for case in ('native', 'capability', 'target_events')]
        # Platform add_executable output remains in its add_subdirectory location.
        roots += [self.cpu / 'platform' / f'anyps5_platform_{name}_test' for name in (
            'kernel', 'content', 'network', 'np', 'audio', 'rtc', 'registration', 'sce',
            'kernel_mutex_threads', 'kernel_priority', 'kernel_round_robin', 'kernel_condition',
            'kernel_static_mutex', 'kernel_events', 'kernel_events_owner', 'kernel_events_native', 'kernel_condition_timeout',
        )]
        for target in roots:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.products / 'native', target)
            target.chmod(0o755)
        for target in (self.cpu / 'tcg/libqemu-x86_64-softmmu.dylib',
                       self.cpu / 'metal-source/core/libs/prx/libSceAgc/libanyps5_metal_agc_host_fixture.dylib'):
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.products / 'libleaf.dylib', target)
        for name in ('cpu-homebrew.elf', 'sce-homebrew.elf', 'sce-module-main.elf', 'SceModuleGuest.prx',
                     'sce-tls.bin', 'cpu-metal-homebrew.elf', 'cpu-memory-metal-homebrew.elf',
                     'platform-service-main.elf', 'PlatformServiceGuest.prx', 'kernel-mmu.bin',
                     'kernel-pagefault.bin', 'ThreadGuest.prx', 'thread-main.elf'):
            (self.cpu / name).write_bytes(b'independent input ' + name.encode())
        for relative in ('sce-crt/crt-receipt.txt', 'sce-crt/sce-crt-main.elf', 'sce-crt/raw/SceCrtGuest.prx',
                         'sce-crt/plain-self/SceCrtGuest.prx', 'sce-main-lifecycle/sce-main-lifecycle.elf'):
            target = self.cpu / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b'independent compiled input role\n')
        utility = self.cpu / 'metal-source/core/libs/prx/libSceAgcDriver/Graphics/Metal/shaders/AnyPS5Utilities.metallib'
        utility.parent.mkdir(parents=True, exist_ok=True)
        utility.write_bytes(b'independent utility fixture\n')
        for folder, names in (
            (self.build / 'native-videoout', ['videoout-' + v + '.bin' for v in ('open', 'status', 'close', 'attribute', 'register', 'rate', 'unregister')]),
            (self.build / 'native-flip', ['target-flip-' + v + '.bin' for v in ('emit', 'wait', 'sequence', 'submit')]),
            (self.cpu / 'platform', [v + '-guest.bin' for v in ('kernel', 'content', 'network', 'np', 'audio', 'rtc')]),
        ):
            for name in names:
                (folder / name).write_bytes(b'independent caller role\n')
        agc = self.cpu / 'metal-source/core/shader/recompiler/MetalReplay/cpu-agc'
        for name in ('write', 'submit', 'shader', 'dispatch', 'acb-write'):
            (agc / ('target-agc-' + name + '.bin')).write_bytes(b'independent AGC caller role\n')
        for relative in ('platform/sce-platform.elf', 'platform/kernel-mutex-threads.elf',
                         'platform/kernel-priority.elf', 'platform/kernel-round-robin.elf',
                         'platform/kernel-condition.elf', 'platform/kernel-static-mutex.elf',
                         'platform/kernel-events.elf', 'platform/thread-main.elf', 'platform/ThreadGuest.prx',
                         'platform/kernel-condition-timeout-posix.elf', 'platform/kernel-condition-timeout-relative.elf',
                         'metal-source/core/shader/recompiler/MetalReplay/cpu-agc/target-agc-events.elf'):
            path = self.cpu / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'independent platform input role\n')

    def test_native_claims_and_transitive_closure_survive_relocation(self):
        result = self.prepare(profile='native')
        manifest = json.loads((self.destination / 'manifest.json').read_text())
        self.assertEqual(manifest['compiled_capabilities'], CAPABILITIES)
        limits = manifest['capability_limits']
        self.assertTrue(limits['main_cli_routes_native_agc'])
        self.assertTrue(limits['main_cli_routes_metal_videoout'])
        self.assertFalse(limits['ps5_game_runtime_ready'])
        self.assertFalse(limits['retail_title_execution_proven'])
        self.assertFalse(limits['actual_title_execution'])
        self.assertEqual(result['acceptance_status'], 'NOT_RUN')
        for name in ('libbridge.dylib', 'libleaf.dylib'):
            self.assertIn('lib/' + name, manifest['files'])
        unrelated = self.root / 'unrelated cwd'
        unrelated.mkdir()
        # Remove every original dylib/artifact location before execution. Real
        # dyld must use relocated package bytes, including the second edge.
        shutil.rmtree(self.build)
        hidden = self.products.with_name(self.products.name + '-temporarily-hidden')
        self.products.rename(hidden)
        try:
            observed = execute('relocated-utility', [self.destination / 'bin/anyps5_cpu_run', '--fixture-utility'], cwd=unrelated)
        finally:
            hidden.rename(self.products)
        self.assertEqual(observed, 'fixture utility and dyld closure resolved\n')
        (self.destination / 'fixtures/AnyPS5Utilities.metallib').unlink()
        with self.assertRaisesRegex(RuntimeError, 'exited 94'):
            execute('missing-relocated-utility', [self.destination / 'bin/anyps5_cpu_run', '--fixture-utility'], cwd=unrelated)

    def test_legacy_default_keeps_native_routes_unclaimed(self):
        self.cache_values.update(CMAKE_HOME_DIRECTORY=str(self.source), ANYPS5_CPU_NATIVE_MODULE_RUNNER='OFF')
        self.write_cache()
        legacy_cpu = self.build / 'core/cpu'
        shutil.copytree(self.cpu, legacy_cpu)
        shutil.copyfile(self.products / 'legacy', legacy_cpu / 'anyps5_cpu_run')
        result = self.prepare()
        manifest = json.loads((self.destination / 'manifest.json').read_text())
        self.assertNotIn('compiled_capabilities', manifest)
        self.assertFalse(manifest['capability_limits']['main_cli_routes_native_agc'])
        self.assertFalse(manifest['capability_limits']['main_cli_routes_metal_videoout'])
        self.assertFalse(manifest['capability_limits']['ps5_game_runtime_ready'])
        self.assertEqual(result['acceptance_status'], 'NOT_RUN')

    def test_compiled_capability_mismatch_does_not_publish(self):
        for name in ('legacy', 'changed', 'unsafe'):
            with self.subTest(compiled_protocol=name):
                shutil.copyfile(self.products / name, self.cpu / 'anyps5_cpu_run')
                with self.assertRaises(RuntimeError) as caught:
                    self.prepare(profile='native')
                self.assertRegex(str(caught.exception).lower(), 'capabilit|native.*runner|runtime.ready')
                self.assertFalse(self.destination.exists())
                self.assertFalse(list(self.root.glob('.diagnostic-package-*')))

    def test_inputs_changed_during_packaging_do_not_publish(self):
        for relative in ('CMakeCache.txt', 'source/core/cpu/sce-module-main.elf'):
            with self.subTest(changed_input=relative):
                target = self.build / relative
                original = target.read_bytes()
                changed = False
                def tamper(name, argv, **kwargs):
                    nonlocal changed
                    value = execute(name, argv, **kwargs)
                    if not changed and Path(argv[0]).name == 'anyps5_cpu_run' and '.diagnostic-package-' in str(argv[0]):
                        target.write_bytes(original + b'tampered during packaging\n')
                        changed = True
                    return value
                try:
                    with self.assertRaisesRegex(RuntimeError, 'changed'):
                        package.prepare_package(self.source, self.build, self.gpu, self.destination,
                                                self.revisions, tamper, profile='native')
                    self.assertTrue(changed, 'control never reached intended tamper point')
                    self.assertFalse(self.destination.exists())
                    self.assertFalse(list(self.root.glob('.diagnostic-package-*')))
                finally:
                    target.write_bytes(original)


class ContractDriftControls(unittest.TestCase):
    """The release contract is a copy of a compiled claim, so it must be checked against the source."""

    def test_contract_equals_the_compiled_emission(self):
        """Pin the release contract to the exact capability object Main.cpp emits (#331).

        A substring search would let a shortened contract claim, an extended compiled claim or a
        renamed key through, so decode the emitted object and compare it field by field. The decode
        is deliberately strict: an expression form it does not recognise leaves the key undecoded,
        which fails the coverage assertion instead of passing silently.
        """
        source = (Path(__file__).resolve().parents[2] / 'core' / 'cpu' / 'src' / 'Main.cpp').read_text()
        marker = '\\"native_module_runner\\":{'
        start = source.index(marker) + len(marker)
        region = source[start:source.index('\\"sce_thread_imports\\"', start)]
        key = r'\\"([^\\]+?)\\":'
        keys = re.findall(key, region)
        self.assertEqual(len(keys), len(set(keys)), 'capability emission repeats a claim key')
        emitted = dict(re.findall(key + r'\\"(.*?)\\"', region, re.S))
        emitted.update({name: value == 'true' for name, value in re.findall(key + r'(true|false)', region)})
        limit_options = {'wall_limit_ms': 'MaxWallMs', 'idle_limit_ms': 'MaxIdleMs'}
        for name, option, default in re.findall(
                key + r'"\s*<<\s*limits\.(\w+)\.value_or\((\d+)\)\s*<<\s*"', region):
            self.assertEqual(limit_options.get(name), option,
                             'limit claim is not bound to its own execution limit option: ' + name)
            emitted[name] = int(default)
        self.assertEqual(emitted, package.NATIVE_RUNNER_CONTRACT)
        # A field the decode does not understand (a number, null, array or punctuated key) would
        # silently vanish from the comparison above, so require it to account for every emitted key.
        self.assertEqual(set(keys), set(emitted),
                         'capability emission contains a field this control cannot decode')
        # Python equates True with 1, so pin each claim's type as well as its value.
        for name, claimed in package.NATIVE_RUNNER_CONTRACT.items():
            self.assertIs(type(emitted[name]), type(claimed), 'capability claim type drifted: ' + name)

    def test_capability_mismatch_names_the_offending_claim(self):
        stale = dict(package.NATIVE_RUNNER_CONTRACT, constraints='no WebAPI2 provider')
        expected = {'native_module_runner': package.NATIVE_RUNNER_CONTRACT}
        self.assertEqual(package.capability_mismatches({'native_module_runner': stale}, expected),
                         ["native_module_runner.constraints='no WebAPI2 provider'"])
        extra = dict(stale, extra='x')
        self.assertEqual(package.capability_mismatches({'native_module_runner': extra}, expected)[1],
                         'native_module_runner.extra=unexpected')
        self.assertEqual(package.capability_mismatches({'native_module_runner': {}}, expected)[0],
                         'native_module_runner.enabled=absent')
        self.assertEqual(package.capability_mismatches({'a': 1}, {'a': True}), ['a=wrong type'])
        self.assertEqual(package.capability_mismatches(
            {'native_module_runner': dict(package.NATIVE_RUNNER_CONTRACT, enabled=1)}, expected),
            ['native_module_runner.enabled=wrong type'])
        self.assertEqual(package.capability_mismatches({'a': 1, 'b': 2}, {'a': 1, 'b': 3}), ['b=2'])
        self.assertEqual(package.capability_mismatches(
            {'native_module_runner': dict(package.NATIVE_RUNNER_CONTRACT)}, expected), [])


if __name__ == '__main__':
    unittest.main()
