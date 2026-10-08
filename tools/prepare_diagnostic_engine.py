#!/usr/bin/env python3
"""Build and accept an isolated ARM64 diagnostic package; never select or run a game.

python3 tools/prepare_diagnostic_engine.py --source <clean AnyPS5 checkout> \
  --revision <40-hex HEAD> --macps-source <clean MacPS checkout> \
  --macps-revision <40-hex HEAD> --output <new external directory> \
  [--dependency-cache <checkout with pinned submodules>]

Every invocation builds fresh. Existing outputs are rejected, never reused or overwritten.
The default legacy profile requires CPU28/GPU22/integration4. --profile native
requires the declared native inventory, relocation and fresh public acceptance.
It builds the explicit --source revision, which must already contain the
qualified production native runner and condition timeout control.
"""
import argparse
from contextlib import AbstractContextManager, contextmanager, nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET

from diagnostic_package import prepare_package

CPU_TESTS = set('''anyps5_system_mmu anyps5_sce_modules anyps5_sce_module_cli
anyps5_sce_main_lifecycle anyps5_cpu_instructions anyps5_guest_threads
anyps5_cpu_contexts anyps5_cpu_loader anyps5_cpu_runtime anyps5_sce_imports
anyps5_sce_loader anyps5_sce_tls anyps5_guest_memory anyps5_sce_memory_imports
anyps5_guest_files anyps5_sce_kernel_imports anyps5_sce_user_imports
anyps5_sce_system_imports anyps5_platform_service_cli anyps5_sce_common_dialog_imports
anyps5_sce_np_local_imports anyps5_sce_net_address_imports anyps5_sce_libc_bootstrap_imports
anyps5_sce_audioout2_imports anyps5_plain_self anyps5_sce_homebrew
anyps5_cpu_homebrew anyps5_cpu_cli'''.split())
GPU_TESTS = set('''anyps5_metal_shader_bridge_contract metal_utility_tests
anyps5_metal_masked_shift_contract anyps5_metal_native_session_own-shutdown
anyps5_metal_native_session_backend-stopped anyps5_metal_native_session_configured-unwind
anyps5_metal_guest_draw_replay anyps5_metal_mesh_replay anyps5_metal_rect_replay
anyps5_metal_image_atomic_replay anyps5_metal_image_1d_replay
anyps5_metal_presentation_replay anyps5_metal_pm4_compute_replay
anyps5_metal_render_state anyps5_metal_guest_replay anyps5_metal_agc_branch_patch
anyps5_metal_agc_predication anyps5_metal_agc_dma_data anyps5_metal_agc_prime_utcl2
anyps5_metal_agc_cond_exec_patch anyps5_metal_agc_register_commands
anyps5_metal_agc_host_exports'''.split())
INTEGRATION_TESTS = set('''anyps5_cpu_metal_guest anyps5_cpu_memory_metal_guest
anyps5_sce_videoout_imports anyps5_sce_native_videoout'''.split())
REQUIRED_TESTS = CPU_TESTS | GPU_TESTS | INTEGRATION_TESTS
# Native qualification retains every legacy control and requires the merged
# platform/owned-memory controls plus the native runner and finite fragments.
# The timeout control requires CPU13's source contract; absent controls fail closed.
NATIVE_REQUIRED_TESTS = REQUIRED_TESTS | set('''anyps5_sce_libc_internal
anyps5_cpu_memory_metal_guest_owned anyps5_cpu_memory_metal_guest_composition
anyps5_owned_memory_metal_mutation anyps5_sce_native_graphics_session
anyps5_metal_shader_correctness_packed anyps5_metal_shader_correctness_half
anyps5_metal_shader_correctness_sdwa anyps5_metal_shader_correctness_lds
anyps5_cpu_agc_native_provider anyps5_cpu_agc_capability anyps5_cpu_agc_target_events
anyps5_platform_kernel anyps5_platform_content anyps5_platform_network
anyps5_platform_np anyps5_platform_audio anyps5_platform_rtc
anyps5_platform_registration anyps5_platform_sce
anyps5_platform_kernel_mutex_threads anyps5_platform_kernel_priority
anyps5_platform_kernel_round_robin anyps5_platform_kernel_condition
anyps5_platform_kernel_static_mutex anyps5_platform_kernel_condition_timeout
anyps5_platform_kernel_events anyps5_platform_kernel_events_owner
anyps5_platform_kernel_events_native cpu09_owner_graph
anyps5_cpu_videoout_native_qualified anyps5_cpu_videoout_packed_hdr
anyps5_cpu_videoout_admission anyps5_cpu_flip_contract
anyps5_cpu_flip_native_qualified anyps5_cpu_flip_native_pending_shutdown
anyps5_native_module_runner_lifecycle anyps5_native_module_runner_close
anyps5_native_module_runner_initializer-failure
anyps5_native_module_runner_qualified-rejection anyps5_native_module_runner_cli
anyps5_metal_optional_sgpr_compute anyps5_metal_optional_sgpr_draw-vertex
anyps5_metal_optional_sgpr_draw-fragment anyps5_metal_optional_sgpr_required
anyps5_metal_normalized_load_replay anyps5_metal_scalar_termination_native
anyps5_metal_scalar_termination_decode anyps5_metal_1d_gather_offset_native
anyps5_metal_1d_gather_offset_rejections'''.split())


def required_tests(profile):
    require(profile in ('legacy', 'native'), 'Unknown diagnostic profile')
    return NATIVE_REQUIRED_TESTS if profile == 'native' else REQUIRED_TESTS


def combined_step(profile):
    return 'combined-native' if profile == 'native' else 'combined54'


def bounded_ninja(output):
    """Cap the owned inner TCG build, whose source currently requests -j8.

    With the outer build at two jobs and the inner at two, at most three
    compiler commands run concurrently. Queries/targets pass through unchanged.
    """
    ninja = shutil.which('ninja')
    require(ninja is not None, 'Ninja executable missing')
    target = output / 'bounded-ninja'
    target.write_text('#!' + sys.executable + '\n' +
        'import os, sys\n' + 'ninja = ' + repr(ninja) + '\n' +
        '''args = []
index = 1
while index < len(sys.argv):
    value = sys.argv[index]
    if value in ('-j', '--jobs'):
        index += 2
        continue
    if value.startswith('-j') or value.startswith('--jobs='):
        index += 1
        continue
    args.append(value)
    index += 1
os.execv(ninja, [ninja, '-j', '2', *args])
''')
    target.chmod(0o755)
    return target


DEPENDENCIES = ['3rdparty/anyps5-tcg', '3rdparty/unicorn', '3rdparty/SDL2',
                '3rdparty/SPIRV-Cross', '3rdparty/SPIRV-Headers',
                '3rdparty/Vulkan-Headers', '3rdparty/glslang']
ALIASES = {'tcg-build': ['cpu-build', 'build.lock'],
           'gpu-validation': ['gpu.lock'], 'title-session': ['title-ui.lock'],
           'git-integration': ['git.lock']}
LOCK_ROOT = Path('/tmp/anyps5-native-swarm-locks')
ACTIVE_LEASES = {}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda: f.read(1048576), b''):
            h.update(b)
    return h.hexdigest()


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def clean_revision(path, commit):
    require(bool(re.fullmatch('[0-9a-f]{40}', commit)), 'Explicit full Git commit required')
    require(Path(git(path, 'rev-parse', '--show-toplevel')).resolve() == path.resolve(), 'Wrong repository: ' + str(path))
    require(git(path, 'rev-parse', 'HEAD') == commit, 'Wrong HEAD: ' + str(path))
    require(not git(path, 'status', '--porcelain', '--untracked-files=all'), 'Repository is dirty: ' + str(path))
    return {'commit': commit, 'tree': git(path, 'rev-parse', 'HEAD^{tree}'),
            'clean_observed': True, 'repository': str(path)}


class Lease(AbstractContextManager):
    """Existing mkdir/owner.json/token protocol, with all historical aliases checked."""
    def __init__(self, names, root=LOCK_ROOT, owner='diagnostic-tools', cleanup=nullcontext):
        self.names, self.root, self.owner = names, Path(root), owner
        self.cleanup = cleanup
        self.token, self.held = uuid.uuid4().hex, []

    def __enter__(self):
        self.root.mkdir(parents=True, exist_ok=True)
        try:
            for name in self.names:
                conflicts = [name, *ALIASES.get(name, [])]
                # CPU/native runs also conflict with an existing retail-title lease.
                if name == 'tcg-build' and 'title-session' not in self.names:
                    conflicts += ['title-session', 'title-ui.lock']
                require(not any((self.root / n).exists() for n in conflicts), 'Lease occupied: ' + name)
                p = self.root / name
                p.mkdir()  # Atomic race against the canonical protocol.
                self.held.append(p)
                ACTIVE_LEASES[str(p)] = self.token
                temporary = p / ('owner-' + self.token + '.tmp')
                temporary.write_text(json.dumps({'token': self.token, 'pid': os.getpid(),
                    'owner': self.owner, 'task': 'prepare-only diagnostic build/fixtures'}) + '\n')
                temporary.rename(p / 'owner.json')
                require(not any((self.root / n).exists() for n in conflicts if n != name), 'Lease occupied: ' + name)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        with self.cleanup():
            errors = []
            for p in reversed(self.held):
                try:
                    metadata = p / 'owner.json'
                    if metadata.exists():
                        owner_record = json.loads(metadata.read_text())
                        require(isinstance(owner_record, dict) and owner_record.get('token') == self.token,
                                'Lease ownership changed: ' + p.name)
                        metadata.unlink()
                    else:
                        # Only our just-created empty directory can lack a published owner.
                        temporary = p / ('owner-' + self.token + '.tmp')
                        if temporary.exists():
                            temporary.unlink()
                    p.rmdir()
                    ACTIVE_LEASES.pop(str(p), None)
                except (OSError, ValueError, RuntimeError) as exc:
                    errors.append(str(exc))
            self.held.clear()
            if errors:
                raise RuntimeError('; '.join(errors))


def parse_ctest_results(xml_path, expected_names):
    try:
        testing = ET.parse(xml_path).getroot().find('Testing')
    except (OSError, ET.ParseError) as exc:
        raise RuntimeError('CTest result missing or malformed') from exc
    require(testing is not None, 'CTest result missing Testing')
    results = [{'name': t.findtext('Name'), 'status': t.get('Status')}
               for t in testing.findall('Test')]
    require(len(results) == len(expected_names) and {r['name'] for r in results} == set(expected_names),
            'CTest required inventory missing, duplicate or unexpected tests')
    require(all(r['status'] == 'passed' for r in results), 'CTest contains failed, skipped or not-run checks')
    return results


class Runner:
    def __init__(self, output, receipt, cleanup=nullcontext):
        self.output, self.receipt, self.cleanup = output, receipt, cleanup
        (output / 'logs').mkdir()

    def save(self):
        (self.output / 'receipt.json').write_text(json.dumps(self.receipt, indent=2) + '\n')

    def __call__(self, name, argv, cwd=None, env=None, timeout=300, expected=0):
        index = len(self.receipt['commands']) + 1
        stdout_path = self.output / 'logs' / f'{index:03d}.stdout.log'
        stderr_path = self.output / 'logs' / f'{index:03d}.stderr.log'
        started, code, failure = time.monotonic(), None, None
        argv = [str(a) for a in argv]
        environment = {k: v for k, v in (env or os.environ).items()
                       if not k.startswith(('LD_', 'DYLD_'))}
        try:
            with stdout_path.open('wb') as out, stderr_path.open('wb') as err:
                process = subprocess.Popen(argv, cwd=cwd, env=environment, stdout=out,
                                           stderr=err, start_new_session=True)
                try:
                    code = process.wait(timeout=timeout)
                except BaseException:
                    # End only this command's owned process group before its lease exits.
                    with self.cleanup():
                        try:
                            os.killpg(process.pid, signal.SIGTERM)
                        except ProcessLookupError:
                            pass
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            pass
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        process.wait()
                    raise
        except (OSError, subprocess.TimeoutExpired) as exc:
            failure = str(exc)
        streams, stdout = {}, ''
        for stream, path in [('stdout', stdout_path), ('stderr', stderr_path)]:
            size, full_sha = path.stat().st_size, digest(path)
            # Keep bounded logs; structured stdout consumers reject oversized output.
            with path.open('rb') as f:
                f.seek(max(0, size - 1048576))
                data = f.read(1048576)
            if size > 1048576:
                path.write_bytes(data)
            if stream == 'stdout':
                stdout = data.decode(errors='replace')
            streams[stream] = {'path': str(path), 'sha256': digest(path),
                'complete_output_sha256': full_sha, 'complete_output_bytes': size,
                'truncated': size > 1048576}
        item = {'name': name, 'argv': argv, 'cwd': str(cwd) if cwd else None,
                'exit_code': code, 'expected_exit_code': expected,
                'seconds': round(time.monotonic() - started, 3), 'timeout_seconds': timeout,
                'failure': failure, 'logs': streams}
        self.receipt['commands'].append(item)
        self.save()
        if not name.startswith('package-'):
            print(json.dumps({'step': name, 'exit_code': code, 'seconds': item['seconds']}), flush=True)
        require(failure is None and code == expected, f'Required command failed: {name}; logs {stdout_path}, {stderr_path}')
        if expected != 0:
            return stdout + stderr_path.read_text(errors='replace')
        return stdout


def clone_exact(run, source, target, commit, label):
    run('clone-' + label, ['git', 'clone', '--no-hardlinks', '--no-checkout', str(source), str(target)])
    run('checkout-' + label, ['git', '-C', str(target), 'checkout', '--detach', commit])
    return clean_revision(target, commit)


def setup_dependencies(run, root, cache):
    records = {}
    for path in DEPENDENCIES:
        pin = git(root, 'rev-parse', 'HEAD:' + path)
        target = root / path
        local = cache / path if cache else None
        if local and (local / '.git').exists():
            # Clone observed objects, then check out the committed pin; never reset cache.
            target.rmdir()
            clone_exact(run, local, target, pin, root.name + '-' + Path(path).name)
        else:
            run('dependency-' + root.name + '-' + Path(path).name,
                ['git', '-C', str(root), 'submodule', 'update', '--init', '--', path], timeout=600)
        records[path] = clean_revision(target, pin)
    return records


def artifact_records(build, profile='legacy'):
    suffixes = {'.a', '.dylib', '.elf', '.prx', '.bin', '.metallib'}
    paths = [p for p in build.rglob('*') if p.is_file() and
             (p.suffix in suffixes or p.name.startswith('anyps5_') and os.access(p, os.X_OK))]
    cpu = build / ('source/core/cpu' if profile == 'native' else 'core/cpu')
    module_object = cpu / 'CMakeFiles/anyps5_sce_modules_tests.dir/tests/SceModulesTests.cpp.o'
    require(module_object.is_file(), 'Compiled module control object missing')
    paths.append(module_object)
    for folder in ('sce-crt', 'sce-main-lifecycle'):
        paths.extend(p for p in (cpu / folder).rglob('*')
                     if p.is_file() and p.suffix in ('.txt', '.json'))
    return {str(p.relative_to(build)): {'path': str(p), 'sha256': digest(p), 'size': p.stat().st_size}
            for p in sorted(paths)}


def write_negative_copy(run, package, output, helper, label, mutate, fragment):
    target = output / ('negative-' + label)
    shutil.copytree(package, target)
    for p in [target, *target.rglob('*')]:
        p.chmod(0o755 if p.is_dir() or os.access(p, os.X_OK) else 0o644)
    manifest = json.loads((target / 'manifest.json').read_text())
    mutate(target, manifest)
    (target / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    result = run('reject-' + label, [helper, target, digest(target / 'manifest.json')], expected=2)
    require(fragment in result, 'Negative control rejected for an unrelated reason: ' + label)


def workflow(args, run, receipt):
    source, app, output = args.source.resolve(), args.macps_source.resolve(), args.output.resolve()
    profile = getattr(args, 'profile', 'legacy')
    expected_tests = required_tests(profile)
    receipt['build_profile'] = profile
    receipt['required_tests'] = sorted(expected_tests)
    engine, gpu, macps, build = [output / n for n in ('engine-source', 'gpu-source', 'macps-source', 'build')]
    with Lease(['git-integration'], cleanup=run.cleanup):
        clone_exact(run, source, engine, args.revision, 'engine')
        clone_exact(run, source, gpu, args.revision, 'gpu')
        clone_exact(run, app, macps, args.macps_revision, 'macps')
        engine_deps = setup_dependencies(run, engine, args.dependency_cache)
        gpu_deps = setup_dependencies(run, gpu, engine)
    revisions = {'AnyPS5': clean_revision(engine, args.revision), 'GPU': clean_revision(gpu, args.revision),
                 '3rdparty/anyps5-tcg': engine_deps['3rdparty/anyps5-tcg'],
                 '3rdparty/unicorn': engine_deps['3rdparty/unicorn']}
    receipt['source_revisions'] = revisions
    receipt['dependencies'] = {'engine': engine_deps, 'gpu': gpu_deps}
    receipt['macps_source'] = clean_revision(macps, args.macps_revision)
    receipt['environment'] = {'machine': os.uname().machine, 'macos': run('macos-version', ['sw_vers', '-productVersion']).strip()}
    require(os.uname().machine == 'arm64', 'Native ARM64 host required')
    receipt['tool_versions'] = {tool: run('version-' + tool, command).strip() for tool, command in
        [('cmake', ['cmake', '--version']), ('ninja', ['ninja', '--version']),
         ('clang', ['clang', '--version']), ('swift', ['swift', '--version']),
         ('glib', ['pkg-config', '--modversion', 'glib-2.0'])]}
    ninja_wrapper = bounded_ninja(output)
    receipt['inner_build_limit'] = {'path': str(ninja_wrapper), 'sha256': digest(ninja_wrapper),
                                  'outer_jobs': 2, 'inner_jobs': 2, 'maximum_compiler_jobs': 3}
    configure_source = engine
    if profile == 'native':
        controls = Path(__file__).with_name('native_diagnostic_controls.cmake').resolve()
        configure_source = output / 'native-controls-source'
        configure_source.mkdir()
        configured_controls = configure_source / 'CMakeLists.txt'
        shutil.copyfile(controls, configured_controls)
        receipt['native_controls_adapter'] = {'path': str(controls), 'sha256': digest(controls),
            'configured_path': str(configured_controls), 'configured_sha256': digest(configured_controls)}
    with Lease(['tcg-build'], cleanup=run.cleanup):
        configure = ['cmake', '-S', configure_source, '-B', build, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DBUILD_TESTING=ON',
            '-DANYPS5_CPU_RUNTIME_ONLY=ON', '-DANYPS5_CPU_BACKEND=TCG',
            '-DANYPS5_CPU_METAL_SOURCE_DIR=' + str(gpu), '-DANYPS5_BUILD_METAL_SHADER_BRIDGE_TESTS=ON',
            '-DtcgNinja:FILEPATH=' + str(ninja_wrapper),
            '-DANYPS5_CPU_NATIVE_MODULE_RUNNER=' + ('ON' if profile == 'native' else 'OFF')]
        if profile == 'native':
            configure.append('-DANYPS5_DIAGNOSTIC_SOURCE:PATH=' + str(engine))
        run('configure', configure)
        run('build', ['cmake', '--build', build, '--parallel', '2'], timeout=1800)
    receipt['build_artifacts'] = artifact_records(build, profile)
    receipt['build_cache_sha256'] = digest(build / 'CMakeCache.txt')
    # Freeze build marker before CPU's separate focused controls; no shared rebuilds.
    (output / 'build-ready.json').write_text(json.dumps({'status': 'BUILT_NOT_VALIDATED',
        'source_revisions': revisions, 'dependencies': receipt['dependencies'],
        'build': str(build), 'cmake_cache_sha256': receipt['build_cache_sha256'], 'artifacts': receipt['build_artifacts'],
        'cpu_native_owner': 'Tools full suite; CPU separate fixed/old/fixed controls'}, indent=2) + '\n')
    inventory = json.loads(run('ctest-inventory', ['ctest', '--test-dir', build, '--show-only=json-v1']))
    require(len(inventory['tests']) == len(expected_tests) and
        {t['name'] for t in inventory['tests']} == expected_tests,
        'CTest required inventory differs from declared ' + profile + ' profile')
    env = dict(os.environ, MTL_DEBUG_LAYER='1', MTL_SHADER_VALIDATION='1',
               ANYPS5_NO_SHADER_CACHE='1', APS5_NO_SHADER_CACHE='1')
    with Lease(['tcg-build', 'gpu-validation', 'title-session'], cleanup=run.cleanup):
        run(combined_step(profile), ['ctest', '--test-dir', build, '-T', 'Test', '--no-tests=error',
            '--output-on-failure', '--parallel', '1'], env=env, timeout=1200)
    tag = (build / 'Testing/TAG').read_text().splitlines()[0]
    receipt['ctest_results'] = parse_ctest_results(build / 'Testing' / tag / 'Test.xml', expected_tests)
    with Lease(['tcg-build'], cleanup=run.cleanup) if profile == 'native' else nullcontext():
        package_info = prepare_package(engine, build, gpu, output / 'package', revisions, run, profile=profile)
    package = Path(package_info['package'])
    receipt['package'] = package_info
    frozen_inputs = {str(Path(r['path']).resolve()): r for r in receipt['build_artifacts'].values()}
    for identity in package_info['input_artifacts'].values():
        original = Path(identity['original_path']).resolve()
        if original.is_relative_to(build):
            frozen = frozen_inputs.get(str(original))
            require(frozen is not None and frozen['sha256'] == identity['original_sha256'],
                    'Package input was not frozen before suite: ' + str(original))
    accept_prepared_package(args, run, receipt)


def accept_prepared_package(args, run, receipt):
    """Finish caller-verified prepared bytes; the CLI always prepares a fresh candidate."""
    output = args.output.resolve()
    profile = getattr(args, 'profile', 'legacy')
    expected_tests = required_tests(profile)
    require(receipt.get('build_profile', 'legacy') == profile, 'Build receipt profile mismatch')
    require(receipt.get('required_tests', sorted(REQUIRED_TESTS)) == sorted(expected_tests),
            'Build receipt required inventory mismatch')
    engine, gpu, macps, build = [output / n for n in ('engine-source', 'gpu-source', 'macps-source', 'build')]
    env = dict(os.environ, MTL_DEBUG_LAYER='1', MTL_SHADER_VALIDATION='1',
               ANYPS5_NO_SHADER_CACHE='1', APS5_NO_SHADER_CACHE='1')
    revisions = receipt['source_revisions']
    engine_deps, gpu_deps = receipt['dependencies']['engine'], receipt['dependencies']['gpu']
    package_info = receipt['package']
    package = Path(package_info['package'])
    require(any(c['name'] == combined_step(profile) and c['exit_code'] == 0 for c in receipt['commands']),
            'Passing combined fixture command missing')
    tag = (build / 'Testing/TAG').read_text().splitlines()[0]
    parse_ctest_results(build / 'Testing' / tag / 'Test.xml', expected_tests)
    if profile == 'native':
        controls = receipt['native_controls_adapter']
        require(digest(controls['path']) == controls['sha256'], 'Native controls adapter changed')
        require(digest(controls['configured_path']) == controls['configured_sha256'] == controls['sha256'],
                'Configured native controls adapter changed')
    require(digest(build / 'CMakeCache.txt') == receipt['build_cache_sha256'], 'Build cache changed')
    wrapper = receipt['inner_build_limit']
    require(digest(wrapper['path']) == wrapper['sha256'], 'Inner build limiter changed')
    for record in receipt['build_artifacts'].values():
        require(digest(record['path']) == record['sha256'], 'Frozen build artifact changed')
    require(digest(package / 'manifest.json') == package_info['manifest_sha256'], 'Prepared manifest changed')
    for relative, record in package_info['artifacts'].items():
        require(digest(package / relative) == record['sha256'], 'Prepared package artifact changed')
    for role, root in [('AnyPS5', engine), ('GPU', gpu)]:
        require(clean_revision(root, args.revision) == revisions[role], 'Source changed before acceptance')
    for root, deps in [(engine, engine_deps), (gpu, gpu_deps)]:
        for relative, identity in deps.items():
            require(clean_revision(root / relative, identity['commit']) == identity, 'Dependency changed before acceptance')
    require(clean_revision(macps, args.macps_revision) == receipt['macps_source'], 'Acceptance source changed')
    scratch = output / 'swift-helper-build'
    with Lease(['tcg-build'], cleanup=run.cleanup):
        run('fresh-LauncherCore', ['swift', 'build', '--package-path', macps, '--scratch-path', scratch,
            '--configuration', 'debug', '--target', 'LauncherCore', '--jobs', '2',
            '-Xswiftc', '-strict-concurrency=complete', '-Xswiftc', '-warnings-as-errors'], timeout=300)
        binary_dir = Path(run('swift-bin-path', ['swift', 'build', '--package-path', macps,
            '--scratch-path', scratch, '--configuration', 'debug', '--show-bin-path']).strip())
        helper = run.output / 'accept-diagnostic-package'
        helper_source = Path(__file__).with_name('accept_diagnostic_package.swift')
        combined_object = binary_dir / 'LauncherCore.o'
        if combined_object.is_file():
            objects, module_dir = [combined_object], binary_dir
        else:
            objects = sorted((binary_dir / 'LauncherCore.build').glob('*.o'))
            module_dir = binary_dir / 'Modules'
        require(bool(objects), 'Fresh LauncherCore objects missing')
        run('link-fresh-accept-helper', ['swiftc', '-parse-as-library', '-strict-concurrency=complete',
            '-warnings-as-errors', '-I', module_dir, helper_source, *objects, '-o', helper])
    receipt['accept_helper'] = {'path': str(helper), 'sha256': digest(helper),
        'adapter_sha256': digest(helper_source), 'macps_commit': args.macps_revision,
        'objects_sha256': {str(p): digest(p) for p in objects}}
    relocation = run.output / 'relocated package with spaces'
    shutil.copytree(package, relocation)
    unrelated = run.output / 'unrelated-cwd'
    unrelated.mkdir()
    with Lease(['tcg-build', 'gpu-validation', 'title-session'], cleanup=run.cleanup):
        for index, argv in enumerate(package_info['relocation_commands']):
            relocated_timeout = 180 if Path(argv[0]).name in {
                'anyps5_metal_normalized_load_replay', 'anyps5_metal_scalar_termination_replay'} else 120
            run('relocation-' + str(index), [str(relocation / a) if a.startswith(('bin/', 'lib/', 'fixtures/')) else a
                for a in argv], cwd=unrelated, env=env, timeout=relocated_timeout)
        accepted = json.loads(run('strict-fresh-accept', [helper, relocation, package_info['manifest_sha256']], cwd=unrelated, timeout=120))
        require(accepted['ps5_game_runtime_ready'] is False and accepted['source_commit'] == args.revision,
                'Accepted package identity or diagnostic capability mismatch')
        receipt['strict_acceptance'] = accepted
        rejected = run('reject-wrong-manifest', [helper, relocation, '0' * 64], expected=2)
        require('differs from the saved accepted selection' in rejected, 'Wrong-pin control unrelated failure')
        write_negative_copy(run, package, run.output, helper, 'dirty-source',
            lambda _, m: m['source_revisions']['AnyPS5'].update(clean_observed=False), 'lacks a clean source revision')
        libs = [n for n in package_info['artifacts'] if n.startswith('lib/') and 'glib' in n]
        require(len(libs) == 1, 'Required transitive GLib closure control missing')
        write_negative_copy(run, package, run.output, helper, 'uncovered-transitive-library',
            lambda _, m: m['files'].pop(libs[0]), 'hash-covered')
    require(digest(package / 'manifest.json') == package_info['manifest_sha256'], 'Package manifest changed')
    for relative, identity in package_info['artifacts'].items():
        require(digest(package / relative) == identity['sha256'] and digest(relocation / relative) == identity['sha256'],
                'Package bytes changed during validation: ' + relative)
    for role, root in [('AnyPS5', engine), ('GPU', gpu)]:
        require(clean_revision(root, args.revision) == revisions[role], 'Source changed during workflow')
    for root, deps in [(engine, engine_deps), (gpu, gpu_deps)]:
        for relative, identity in deps.items():
            require(clean_revision(root / relative, identity['commit']) == identity, 'Dependency changed during workflow')
    require(clean_revision(macps, args.macps_revision) == receipt['macps_source'], 'MacPS acceptance source changed')
    # Seal only the accepted package. Logs, build outputs and controls remain external.
    for p in package.rglob('*'):
        p.chmod(0o555 if p.is_dir() or os.access(p, os.X_OK) else 0o444)
    package.chmod(0o555)
    package_info['acceptance_status'] = 'PASS'
    package_info['relocation_status'] = 'PASS'
    for record in receipt['build_artifacts'].values():
        require(digest(record['path']) == record['sha256'], 'Build artifact changed after freeze: ' + record['path'])
    receipt.update(status='PASS', package_sealed=True, leases_released=True,
        failures=[], skips=[], unknowns=[('Native runner implements a qualified diagnostic provider subset; '
            'actual title execution and playability unproven') if profile == 'native' else
            'Production Main does not wire NativeMetalSession/AGC providers; game rendering and playability unproven'],
        manual_selection={'action': 'MacPS Settings gear > Choose AnyPS5 Runtime… > select package folder',
            'package': str(package), 'manifest_sha256': package_info['manifest_sha256'],
            'engine_selection_performed': False, 'game_runtime_ready': False})
    run.save()


@contextmanager
def cli_cancellation():
    """Route TERM through command/lease unwinding only while the CLI runs.

    First TERM is deferred inside TERM/KILL/reap or token-qualified release,
    even when timeout or SIGINT started cleanup. Repeated TERM is ignored.
    SIGKILL cannot be made cleanup-safe. Imported helpers retain caller handlers.
    """
    previous = signal.getsignal(signal.SIGTERM)
    requested, pending, protected = False, False, 0

    def terminate(signum, _frame):
        nonlocal requested, pending
        if not requested:
            requested = True
            if protected:
                pending = True
            else:
                raise KeyboardInterrupt('Terminated by ' + signal.Signals(signum).name)

    @contextmanager
    def cleanup():
        nonlocal protected, pending
        protected += 1
        try:
            yield
        finally:
            protected -= 1
            if not protected and pending:
                pending = False
                raise KeyboardInterrupt('Terminated by SIGTERM')

    try:
        signal.signal(signal.SIGTERM, terminate)
        yield cleanup
    finally:
        signal.signal(signal.SIGTERM, previous)


def main():
    with cli_cancellation() as cleanup:
        parser = argparse.ArgumentParser(description=__doc__)
        for name in ('source', 'macps-source', 'output'):
            parser.add_argument('--' + name, type=Path, required=True)
        for name in ('revision', 'macps-revision'):
            parser.add_argument('--' + name, required=True)
        parser.add_argument('--dependency-cache', type=Path)
        parser.add_argument('--profile', choices=('legacy', 'native'), default='legacy',
                            help='Explicit native opt-in; requires the full native source and control contracts')
        args = parser.parse_args()
        receipt = {'schema_version': 1, 'status': 'NOT_RUN', 'commands': [],
            'title_execution': False, 'actual_title_execution': False, 'app_mutation': False, 'engine_selection': False,
            'workflow_sha256': digest(Path(__file__)), 'package_adapter_sha256': digest(Path(__file__).with_name('diagnostic_package.py'))}
        output, run = args.output.resolve(), None
        try:
            clean_revision(args.source.resolve(), args.revision)
            clean_revision(args.macps_source.resolve(), args.macps_revision)
            require(not any(output.is_relative_to(p.resolve()) for p in (args.source, args.macps_source)), 'Output must be outside source repositories')
            require(not output.exists() and not args.output.is_symlink(), 'Output already exists')
            output.mkdir(parents=True)
            run = Runner(output, receipt, cleanup=cleanup)
            workflow(args, run, receipt)
            print(json.dumps({'status': 'PASS', 'receipt': str(output / 'receipt.json'),
                             'package': receipt['package']['package']}))
            return 0
        except BaseException as exc:
            receipt.update(status='FAILED', failure=str(exc), package_sealed=False,
                           required_checks_complete=False, leases_released=not ACTIVE_LEASES, outstanding_leases=ACTIVE_LEASES.copy())
            if run:
                run.save()
            print(json.dumps(receipt if run is None else {'status': 'FAILED', 'failure': str(exc),
                'receipt': str(output / 'receipt.json')}), file=sys.stderr)
            return 1


if __name__ == '__main__':
    sys.exit(main())
