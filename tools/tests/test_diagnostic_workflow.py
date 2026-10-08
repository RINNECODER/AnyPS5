"""Independent fail-closed controls for the prepare-only package boundary.

Contract: unsafe or mismatched identities cannot reach a build or publish a
package; a skipped/missing CTest case cannot become a passing receipt; existing
lease owners must survive a competing invocation. These regressions have no
existing workflow coverage. Temporary Git repositories exercise the real CLI,
and synthetic CTest XML exercises its external result protocol without running
native tests or adding a production-only test hook.
"""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET


SCRIPT = Path(__file__).resolve().parents[1] / "prepare_diagnostic_engine.py"


def git(repo, *args):
    return subprocess.check_output(
        ["git", "-C", str(repo), *args], text=True, stderr=subprocess.STDOUT
    ).strip()


def clean_repository(path):
    path.mkdir()
    git(path, "init", "-q")
    git(path, "config", "user.name", "Workflow control")
    git(path, "config", "user.email", "control@example.invalid")
    (path / "tracked.txt").write_text("committed input\n")
    git(path, "add", "tracked.txt")
    git(path, "commit", "-qm", "fixture input")
    return git(path, "rev-parse", "HEAD")


class DiagnosticWorkflowControls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="diagnostic-control-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "engine"
        self.macps = self.root / "macps"
        self.revision = clean_repository(self.source)
        self.macps_revision = clean_repository(self.macps)
        self.output = self.root / "output"

    def invoke(self, **overrides):
        arguments = {
            "source": self.source,
            "revision": self.revision,
            "macps-source": self.macps,
            "macps-revision": self.macps_revision,
            "output": self.output,
        }
        arguments.update(overrides)
        command = [sys.executable, str(SCRIPT)]
        for name, value in arguments.items():
            command.extend(["--" + name, str(value)])
        return subprocess.run(command, capture_output=True, text=True, timeout=15)

    def assert_rejected(self, result, diagnostic, output=None, forbidden_output=False):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(diagnostic, result.stdout + result.stderr)
        destination = output or self.output
        if forbidden_output:
            self.assertFalse(destination.exists(), "rejected input created output")
        else:
            failure = json.loads(result.stderr)
            self.assertEqual(failure["status"], "FAILED")
            self.assertEqual(failure["commands"], [], "rejected input reached execution")
            self.assertFalse(destination.exists(), "rejected input created output")

    def test_dirty_engine_and_macps_sources_are_rejected_before_build(self):
        for repository in (self.source, self.macps):
            with self.subTest(repository=repository.name):
                untracked = repository / "uncommitted.txt"
                untracked.write_text("must not enter package identity\n")
                try:
                    self.assert_rejected(self.invoke(), "Repository is dirty")
                finally:
                    untracked.unlink()

    def test_exact_full_revision_is_required_for_both_repositories(self):
        for flag, revision in (
            ("revision", self.revision),
            ("macps-revision", self.macps_revision),
        ):
            for value, diagnostic in (
                (revision[:12], "Explicit full Git commit required"),
                ("f" * 40, "Wrong HEAD"),
            ):
                with self.subTest(flag=flag, value=value):
                    self.assert_rejected(self.invoke(**{flag: value}), diagnostic)

    def test_output_inside_either_source_is_rejected_before_mutation(self):
        for repository in (self.source, self.macps):
            with self.subTest(repository=repository.name):
                output = repository / "package-output"
                self.assert_rejected(
                    self.invoke(output=output), "Output must be outside source repositories",
                    output, forbidden_output=True,
                )
                self.assertEqual(git(repository, "status", "--porcelain"), "")

    def test_existing_output_is_preserved(self):
        self.output.mkdir()
        sentinel = self.output / "existing-package.txt"
        sentinel.write_bytes(b"sealed output\x00")
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Output already exists", result.stdout + result.stderr)
        self.assertEqual(sentinel.read_bytes(), b"sealed output\x00")
        self.assertEqual(sorted(path.name for path in self.output.iterdir()), [sentinel.name])


class ResultAndLeaseControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("diagnostic_workflow_controls", SCRIPT)
        sys.path.insert(0, str(SCRIPT.parent))
        cls.workflow = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.workflow
        spec.loader.exec_module(cls.workflow)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="diagnostic-protocol-control-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def results(self, rows):
        site = ET.Element("Site")
        testing = ET.SubElement(site, "Testing")
        inventory = ET.SubElement(testing, "TestList")
        for name, status in rows:
            ET.SubElement(inventory, "Test").text = name
            result = ET.SubElement(testing, "Test", Status=status)
            ET.SubElement(result, "Name").text = name
            ET.SubElement(result, "FullName").text = name
            ET.SubElement(result, "Results")
        path = self.root / "Test.xml"
        ET.ElementTree(site).write(path, encoding="utf-8", xml_declaration=True)
        return path

    def test_ctest_requires_exact_passing_inventory(self):
        expected = ["cpu_contract", "native_fixture"]
        valid = [(name, "passed") for name in expected]
        accepted = self.workflow.parse_ctest_results(self.results(valid), expected)
        self.assertEqual({row["name"] for row in accepted}, set(expected))
        self.assertTrue(all(row["status"] == "passed" for row in accepted))
        invalid = {
            "skipped": [("cpu_contract", "passed"), ("native_fixture", "notrun")],
            "failed": [("cpu_contract", "passed"), ("native_fixture", "failed")],
            "unknown status": [("cpu_contract", "passed"), ("native_fixture", "unknown")],
            "missing": [("cpu_contract", "passed")],
            "extra": valid + [("unrequired_probe", "passed")],
            "duplicate": valid + [("native_fixture", "passed")],
        }
        for case, rows in invalid.items():
            with self.subTest(case=case):
                with self.assertRaises(RuntimeError):
                    self.workflow.parse_ctest_results(self.results(rows), expected)
        with self.assertRaises(RuntimeError):
            self.workflow.parse_ctest_results(self.root / "missing.xml", expected)

    def test_existing_canonical_and_alias_leases_are_never_stolen(self):
        for name in ("build.lock", "tcg-build"):
            with self.subTest(name=name):
                occupied = self.root / name
                occupied.mkdir()
                owner = occupied / "owner.json"
                original = b'{"token":"another-owner","pid":12345}\n'
                owner.write_bytes(original)
                with self.assertRaisesRegex(RuntimeError, "Lease occupied"):
                    with self.workflow.Lease(["tcg-build"], self.root, "control"):
                        self.fail("occupied lease was acquired")
                self.assertEqual(owner.read_bytes(), original)
                self.assertEqual(sorted(path.name for path in self.root.iterdir()), [name])
                owner.unlink()
                occupied.rmdir()

    def test_own_lease_releases_and_changed_owner_survives(self):
        with self.workflow.Lease(["tcg-build"], self.root, "control"):
            self.assertTrue((self.root / "tcg-build" / "owner.json").is_file())
        self.assertEqual(list(self.root.iterdir()), [])
        owner = self.root / "tcg-build" / "owner.json"
        replacement = None
        with self.assertRaisesRegex(RuntimeError, "Lease ownership changed"):
            with self.workflow.Lease(["tcg-build"], self.root, "control"):
                replacement = json.loads(owner.read_text())
                replacement["token"] = "another-owner"
                owner.write_text(json.dumps(replacement))
        self.assertEqual(json.loads(owner.read_text()), replacement)

    def test_changed_later_lease_does_not_leak_earlier_owned_lease(self):
        """Changed tokens or valid non-object JSON must not abort other releases.

        Review found array/null owner metadata raised AttributeError before the
        remaining owned leases could release; changed-token coverage missed it.
        """
        for case, replacement in (("changed", {"token": "another-owner"}),
                                  ("array", []), ("null", None)):
            with self.subTest(owner_record=case):
                locks = self.root / case
                owner = locks / "gpu-validation" / "owner.json"
                with self.assertRaisesRegex(RuntimeError, "Lease ownership changed"):
                    with self.workflow.Lease(["tcg-build", "gpu-validation"], locks, "control"):
                        owner.write_text(json.dumps(replacement))
                self.assertFalse((locks / "tcg-build").exists())
                self.assertEqual(json.loads(owner.read_text()), replacement)

    def runner(self):
        receipt = {"commands": []}
        return self.workflow.Runner(self.root, receipt), receipt

    def test_cli_restores_previous_sigterm_handler_on_success_and_failure(self):
        """Helpers retain caller handlers; main restores them on every exit.

        A TERM just before completion must reject PASS even without another
        command wait. Earlier success/failure/parser cases missed that boundary.
        """
        previous = signal.getsignal(signal.SIGTERM)
        handler = lambda *_: None
        signal.signal(signal.SIGTERM, handler)
        self.addCleanup(signal.signal, signal.SIGTERM, previous)
        spec = importlib.util.spec_from_file_location('scoped_cli_control', SCRIPT)
        scoped_workflow = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(scoped_workflow)
        self.assertIs(signal.getsignal(signal.SIGTERM), handler, 'helper import changed SIGTERM handling')
        source, macps = self.root / 'source', self.root / 'macps'
        revision, app_revision = clean_repository(source), clean_repository(macps)
        for outcome in ('success', 'failure', 'TERM'):
            with self.subTest(outcome=outcome):
                output = self.root / outcome
                argv = [str(SCRIPT), '--source', str(source), '--revision', revision,
                        '--macps-source', str(macps), '--macps-revision', app_revision,
                        '--output', str(output)]

                active_handlers = []

                def exercise(args, run, receipt):
                    active_handlers.append(signal.getsignal(signal.SIGTERM))
                    if outcome == 'failure':
                        raise RuntimeError('controlled workflow failure')
                    receipt['package'] = {'package': str(output / 'package')}
                    if outcome == 'TERM':
                        os.kill(os.getpid(), signal.SIGTERM)

                stdout, stderr = io.StringIO(), io.StringIO()
                with patch.object(sys, 'argv', argv), patch.object(scoped_workflow, 'workflow', exercise), \
                     redirect_stdout(stdout), redirect_stderr(stderr):
                    self.assertEqual(scoped_workflow.main(), int(outcome != 'success'))
                if outcome == 'TERM':
                    self.assertEqual(stdout.getvalue(), '', 'TERM request published PASS')
                    self.assertEqual(json.loads(stderr.getvalue())['status'], 'FAILED')
                    self.assertEqual(json.loads((output / 'receipt.json').read_text())['status'], 'FAILED')
                self.assertEqual(len(active_handlers), 1)
                self.assertIsNot(active_handlers[0], handler, 'CLI did not install cancellation handling')
                self.assertIs(signal.getsignal(signal.SIGTERM), handler)
        with patch.object(sys, 'argv', [str(SCRIPT)]), redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as exit_status:
                scoped_workflow.main()
        self.assertEqual(exit_status.exception.code, 2)
        self.assertIs(signal.getsignal(signal.SIGTERM), handler, 'parser exit leaked handler')

    @unittest.skipUnless(os.name == 'posix', 'Owned command groups require POSIX signals')
    def test_real_sigterm_stops_owned_group_before_token_qualified_lease_release(self):
        """First/repeated TERM must not interrupt cleanup, whatever started it.

        Contract: TERM/KILL/reap precedes token-qualified release, which must
        also finish under TERM. Regression: first TERM raises inside timeout or
        SIGINT teardown (or release), bypassing KILL/reap or stranding leases.
        Prior coverage always sent TERM first, masking these cancellation holes.
        Entry-window cases deliver actual TERM before cleanup/release work:
        the old asynchronous handler escapes before protection is established.
        Existing shutdown/read handshakes only cover already-entered cleanup.
        A fixture trace provides scheduling, not a production-only test seam.
        Real CLI/Runner/Lease and process groups own this proof; readiness and
        observation stay in the fixture, without production-only test seams.
        """
        for initiator, target in (('timeout', 'wait'), ('SIGINT', 'wait'),
                                  ('SIGTERM', 'wait'), ('timeout', 'release'),
                                  ('timeout', 'cleanup-entry'), ('timeout', 'lease-entry')):
            with self.subTest(initiator=initiator, target=target):
                folder = self.root / (initiator + '-' + target)
                folder.mkdir()
                self.exercise_sigterm_cleanup(folder, initiator, target)

    def exercise_sigterm_cleanup(self, root, initiator, target):
        locks = root / 'private-locks'
        ready = root / 'ready.json'
        source, macps = root / 'source', root / 'macps'
        revision, app_revision = clean_repository(source), clean_repository(macps)
        foreign = locks / 'foreign-control'
        foreign.mkdir(parents=True)
        foreign_bytes = b'{"token":"foreign-untouched","pid":12345}\n'
        (foreign / 'owner.json').write_bytes(foreign_bytes)
        child = textwrap.dedent(f'''
            import json, os, pathlib, signal, time
            root = pathlib.Path({str(root)!r})
            locks = pathlib.Path({str(locks)!r})
            def terminate(*_):
                owners = {{name: json.loads((locks / name / 'owner.json').read_text())['token']
                          for name in ('tcg-build', 'gpu-validation')}}
                (root / 'shutdown-child.json').write_text(json.dumps(owners))
            signal.signal(signal.SIGTERM, terminate)
            (root / 'child-ready').touch()
            while True: time.sleep(0.05)
        ''')
        command = textwrap.dedent(f'''
            import json, os, pathlib, signal, subprocess, sys, time
            root = pathlib.Path({str(root)!r})
            locks = pathlib.Path({str(locks)!r})
            def terminate(*_):
                owners = {{name: json.loads((locks / name / 'owner.json').read_text())['token']
                          for name in ('tcg-build', 'gpu-validation')}}
                (root / 'shutdown-parent.json').write_text(json.dumps(owners))
            signal.signal(signal.SIGTERM, terminate)
            child = subprocess.Popen([sys.executable, '-c', {child!r}])
            (root / 'group.tmp').write_text(json.dumps({{'group': os.getpid(), 'child': child.pid}}))
            (root / 'group.tmp').rename(root / 'group.json')
            while not (root / 'child-ready').exists(): time.sleep(0.01)
            (root / 'ready.tmp').write_text((root / 'group.json').read_text())
            (root / 'ready.tmp').rename(root / 'ready.json')
            while True: time.sleep(0.05)
        ''')
        supervisor_code = textwrap.dedent(f'''
            import json, pathlib, signal, subprocess, sys, time
            sys.path.insert(0, {str(SCRIPT.parent)!r})
            import prepare_diagnostic_engine as workflow
            root = pathlib.Path({str(root)!r})
            locks = pathlib.Path({str(locks)!r})
            original_exit = workflow.Lease.__exit__
            def observe_release(lease, *args):
                pids = json.loads((root / 'ready.json').read_text())
                states = {{role: subprocess.run(['ps', '-o', 'stat=', '-p', str(pid)],
                          capture_output=True, text=True).stdout.strip()
                          for role, pid in pids.items()}}
                (root / 'before-release.json').write_text(json.dumps({{
                    'states': states, 'owned_present': (locks / 'tcg-build/owner.json').exists()}}))
                if {target!r} == 'release':
                    original_read = pathlib.Path.read_text
                    def release_read(path, *a, **kw):
                        if path == locks / 'tcg-build/owner.json':
                            (root / 'release-ready').touch()
                            deadline = time.monotonic() + 10
                            while not (root / 'release-signaled').exists():
                                if time.monotonic() > deadline:
                                    raise RuntimeError('release signal handshake timed out')
                                time.sleep(0.01)
                        return original_read(path, *a, **kw)
                    pathlib.Path.read_text = release_read
                return original_exit(lease, *args)
            workflow.Lease.__exit__ = observe_release
            if {target!r} in ('cleanup-entry', 'lease-entry'):
                def entry_signal(frame, event, arg):
                    cleanup_entry = ({target!r} == 'cleanup-entry' and
                                     frame.f_code is workflow.Runner.__call__.__code__ and
                                     event == 'line' and
                                     isinstance(sys.exception(), subprocess.TimeoutExpired) and
                                     sys.exception().timeout == frame.f_locals.get('timeout'))
                    lease_entry = ({target!r} == 'lease-entry' and
                                   frame.f_code is original_exit.__code__ and event == 'line')
                    if cleanup_entry or lease_entry:
                        sys.settrace(None)
                        pids = json.loads((root / 'ready.json').read_text())
                        states = {{role: subprocess.run(['ps', '-o', 'stat=', '-p', str(pid)],
                                  capture_output=True, text=True).stdout.strip()
                                  for role, pid in pids.items()}}
                        (root / 'entry-signal.json').write_text(json.dumps({{
                            'states': states,
                            'owned_present': (locks / 'tcg-build/owner.json').exists()}}))
                        # Real delivery in the supervisor's main thread, before
                        # the original cleanup factory or lease exit body runs.
                        import os
                        os.kill(os.getpid(), signal.SIGTERM)
                    return entry_signal
                sys.settrace(entry_signal)
            def harmless_workflow(args, run, receipt):
                # Thread the CLI context when available; retain baseline replay.
                lease_options = {{'cleanup': run.cleanup}} if hasattr(run, 'cleanup') else {{}}
                with workflow.Lease(['tcg-build', 'gpu-validation'], locks,
                                    **lease_options) as lease:
                    receipt['test_owned_token'] = lease.token
                    (locks / 'gpu-validation/owner.json').write_text('{{"token":"replacement-foreign"}}\\n')
                    run('package-signal-control', [sys.executable, '-c', {command!r}],
                        timeout={2 if initiator == 'timeout' else 60})
            workflow.workflow = harmless_workflow
            sys.argv = [{str(SCRIPT)!r}, '--source', {str(source)!r}, '--revision', {revision!r},
                        '--macps-source', {str(macps)!r}, '--macps-revision', {app_revision!r},
                        '--output', {str(root / 'output')!r}]
            sys.exit(workflow.main())
        ''')
        supervisor = subprocess.Popen([sys.executable, '-B', '-c', supervisor_code],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        pids = None

        def wait_ready(path):
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if path.exists():
                    return
                if supervisor.poll() is not None:
                    break
                time.sleep(0.01)
            output, error = supervisor.communicate(timeout=2) if supervisor.poll() is not None else ('', '')
            self.fail(f'supervisor exited or readiness timed out: {path.name}; '
                      f'exit={supervisor.poll()}, owned_lease={(locks / "tcg-build").exists()}; '
                      f'{output}{error}')

        try:
            wait_ready(ready)
            pids = json.loads(ready.read_text())
            if initiator != 'timeout':
                supervisor.send_signal(getattr(signal, initiator))
            if target.endswith('entry'):
                wait_ready(root / 'entry-signal.json')
                entry = json.loads((root / 'entry-signal.json').read_text())
                self.assertTrue(entry['owned_present'])
                if target == 'cleanup-entry':
                    for state in entry['states'].values():
                        self.assertTrue(state and not state.startswith('Z'),
                                        'entry signal did not arrive while owned processes executed')
            else:
                wait_ready(root / 'shutdown-parent.json')
                wait_ready(root / 'shutdown-child.json')
                if target == 'release':
                    wait_ready(root / 'release-ready')
                else:
                    self.assertIsNone(supervisor.poll(), 'supervisor left cleanup before TERM')
                # For timeout/SIGINT this is the FIRST external TERM, after both
                # group members acknowledged shutdown while retaining their leases.
                supervisor.send_signal(signal.SIGTERM)
                time.sleep(0.1)  # Separate deliveries; do not rely on coalesced signals.
                supervisor.send_signal(signal.SIGTERM)
                if target == 'release':
                    (root / 'release-signaled').touch()
            output, error = supervisor.communicate(timeout=12)
            self.assertEqual(supervisor.returncode, 1, output + error)
            before = json.loads((root / 'before-release.json').read_text())
            self.assertTrue(before['owned_present'])
            self.assertEqual(before['states']['group'], '', 'command leader was not reaped')
            self.assertTrue(not before['states']['child'] or before['states']['child'].startswith('Z'),
                            'owned descendant still executing when lease release began')
            receipt = json.loads((root / 'output/receipt.json').read_text())
            for role in ('parent', 'child'):
                owners = json.loads((root / f'shutdown-{role}.json').read_text())
                self.assertEqual(owners['tcg-build'], receipt['test_owned_token'])
                self.assertEqual(owners['gpu-validation'], 'replacement-foreign')
            self.assertFalse((locks / 'tcg-build').exists(), 'owned lease stranded after TERM')
            self.assertEqual((locks / 'gpu-validation/owner.json').read_bytes(),
                             b'{"token":"replacement-foreign"}\n')
            self.assertEqual((foreign / 'owner.json').read_bytes(), foreign_bytes)
            self.assertEqual(receipt['status'], 'FAILED')
            self.assertFalse(receipt['leases_released'], 'foreign token misreported as released')
            self.assertEqual(receipt['outstanding_leases'],
                             {str(locks / 'gpu-validation'): receipt['test_owned_token']})
        finally:
            # A baseline failure still must not strand the test's owned group.
            group_record = root / 'group.json'
            if pids is None and group_record.exists():
                pids = json.loads(group_record.read_text())
            if pids:
                try:
                    os.killpg(pids['group'], signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if supervisor.poll() is None:
                supervisor.kill()
            supervisor.communicate(timeout=5)

    def test_runner_keeps_warning_stderr_out_of_json_stdout(self):
        """Real subprocess output catches the reviewed merged-stream JSON risk."""
        run, receipt = self.runner()
        result = run("package-json-control", [sys.executable, "-c",
            "import sys; print('{\"accepted\":true}'); print('native warning', file=sys.stderr)"])
        self.assertEqual(json.loads(result), {"accepted": True})
        command = receipt["commands"][0]
        self.assertEqual(command["exit_code"], 0)
        self.assertEqual(Path(command["logs"]["stderr"]["path"]).read_text(), "native warning\n")

    def test_runner_timeout_stops_owned_descendant_before_return(self):
        """An unreaped child can mutate artifacts after the timed-out lease releases.

        No prior test exercises the real Runner process group. This test starts
        only its own two Python processes and observes their external sentinel.
        """
        run, receipt = self.runner()
        sentinel = self.root / "late-child-completion"
        child = (
            "import pathlib, time; print('child-ready', flush=True); "
            "time.sleep(2); pathlib.Path(" + repr(str(sentinel)) + ").write_text('completed')"
        )
        parent = (
            "import subprocess, sys, time; "
            "subprocess.Popen([sys.executable, '-c', " + repr(child) + "]); time.sleep(10)"
        )
        with self.assertRaisesRegex(RuntimeError, "Required command failed"):
            run("package-timeout-control", [sys.executable, "-c", parent], timeout=0.2)
        command = receipt["commands"][0]
        self.assertIsNotNone(command["failure"])
        self.assertNotEqual(command["exit_code"], 0)
        self.assertIn("child-ready", Path(command["logs"]["stdout"]["path"]).read_text())
        time.sleep(2.2)
        self.assertFalse(sentinel.exists(), "timed-out command left its owned child running")

    def test_runner_interruption_stops_owned_descendant_before_propagating(self):
        """KeyboardInterrupt must clean up the same real group as a timeout.

        Timeout-only coverage missed the reviewed BaseException cleanup gap.
        Only wait interruption is injected; both owned subprocesses are real.
        """
        run, _ = self.runner()
        ready = self.root / "child-started"
        sentinel = self.root / "interrupted-child-completion"
        child = (
            "import pathlib, time; pathlib.Path(" + repr(str(ready)) + ").touch(); "
            "time.sleep(1); pathlib.Path(" + repr(str(sentinel)) + ").write_text('completed')"
        )
        parent = (
            "import subprocess, sys, time; "
            "subprocess.Popen([sys.executable, '-c', " + repr(child) + "]); time.sleep(10)"
        )
        original_wait = subprocess.Popen.wait
        owned = []

        def interrupted_wait(process, *args, **kwargs):
            if not owned:
                owned.append(process)
                deadline = time.monotonic() + 2
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(ready.exists(), "child did not start before interruption")
                raise KeyboardInterrupt("test-owned interruption")
            return original_wait(process, *args, **kwargs)

        try:
            with patch.object(subprocess.Popen, "wait", interrupted_wait):
                with self.assertRaisesRegex(KeyboardInterrupt, "test-owned interruption"):
                    run("package-interruption-control", [sys.executable, "-c", parent])
            self.assertIsNotNone(owned[0].poll(), "interrupted parent was not reaped")
            time.sleep(1.2)
            self.assertFalse(sentinel.exists(), "interrupted command left its owned child running")
        finally:
            # On a regression, clean up only this test's isolated process group.
            if owned:
                try:
                    os.killpg(owned[0].pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                original_wait(owned[0])


if __name__ == "__main__":
    unittest.main()
