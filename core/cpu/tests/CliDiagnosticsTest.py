"""Strict CLI diagnostics with independently scripted native stderr fixtures.

The subprocess fixture exercises the complete retained CLI assertions; it does
not execute native code or establish guest/game compatibility.
"""

import importlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


API_BANNER = "2026-10-07 07:06:48.759 anyps5_cpu_run[50824:141489416] Metal API Validation Enabled"
GPU_BANNER = "2026-10-07 07:06:48.759 anyps5_cpu_run[50824:141489416] Metal GPU Validation Enabled"
EVENT = {"schema_version": 1, "event": "guest_exit", "exit_code": 0}
ENABLED = {"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1"}
HERE = Path(__file__).resolve().parent


class DiagnosticStreamContract(unittest.TestCase):
    def parse(self, stderr, environment=None, platform="darwin"):
        helper = importlib.import_module("CliDiagnostics")
        return helper.parse_diagnostics(stderr,
                                        environment=ENABLED if environment is None else environment,
                                        platform=platform)

    def test_exact_enabled_announcements_preserve_diagnostic_records(self):
        record = json.dumps(EVENT)
        for prefix, environment in (("", {}),
                                    (API_BANNER + "\n", {"MTL_DEBUG_LAYER": "1"}),
                                    (GPU_BANNER + "\n", {"MTL_SHADER_VALIDATION": "1"}),
                                    (API_BANNER + "\n" + GPU_BANNER + "\n", ENABLED)):
            with self.subTest(prefix=prefix):
                self.assertEqual(self.parse(prefix + record + "\n", environment), [EVENT])

    def test_unrecognized_or_disabled_native_output_is_rejected(self):
        record = json.dumps(EVENT)
        cases = [
            (API_BANNER, {}, "darwin"),
            (GPU_BANNER, {"MTL_DEBUG_LAYER": "1"}, "darwin"),
            (API_BANNER, {"MTL_DEBUG_LAYER": "0"}, "darwin"),
            (GPU_BANNER, {"MTL_SHADER_VALIDATION": "0"}, "darwin"),
            (API_BANNER, ENABLED, "linux"),
            (GPU_BANNER, ENABLED, "win32"),
            (API_BANNER + " unexpected suffix", ENABLED, "darwin"),
            (API_BANNER.replace("Enabled", "Error: device lost"), ENABLED, "darwin"),
            (API_BANNER.replace("anyps5_cpu_run", "other_process"), ENABLED, "darwin"),
            ("Metal API Validation Enabled", ENABLED, "darwin"),
            ("fatal native teardown error", ENABLED, "darwin"),
        ]
        for line, environment, platform in cases:
            with self.subTest(line=line, environment=environment, platform=platform):
                with self.assertRaises(Exception):
                    self.parse(line + "\n" + record + "\n", environment, platform)

    def test_corrupt_or_missing_diagnostics_cannot_be_hidden_by_banners(self):
        cases = ["", API_BANNER + "\n" + GPU_BANNER,
                 '{"schema_version":1,"event":',
                 '{"schema_version":2,"event":"guest_exit","exit_code":0}',
                 '{"schema_version":true,"event":"guest_exit","exit_code":0}',
                 '{"schema_version":1.0,"event":"guest_exit","exit_code":0}',
                 '{"schema_version":1,"event":"guest_exit","exit_code":NaN}',
                 '{"schema_version":1,"event":"guest_exit","exit_code":Infinity}',
                 '{"event":"guest_exit","exit_code":0}', "null", "[]", "42"]
        for payload in cases:
            with self.subTest(payload=payload):
                with self.assertRaises(Exception):
                    self.parse(API_BANNER + "\n" + payload)


# The fixture independently chooses diagnostic streams from CLI arguments. It
# supplies no implementation of the parser or retained script assertions.
RUNNER = r'''#!{python}
import json
import os
from pathlib import Path
import sys

a = sys.argv[1:]
mutation = os.environ.get("CLI_FIXTURE_MUTATION", "")
profile = os.environ.get("CLI_FIXTURE_PROFILE", "sce")
if a and a[0] == "--diagnostics-json":
    a = a[1:]
status = 0
error = None
main = ""
if not a or ("--sce-module" in a and len(a) == 1):
    error = ("invalid_arguments", "--sce-module requires an argument")
elif "--inspect-sce-json" in a:
    error = ("invalid_arguments", "--sce-module cannot be inspected")
elif a[0] != "--sce-module":
    error = ("unsupported_executable", "DT_NEEDED guest module loading is unsupported")
else:
    module = Path(a[1])
    main = a[2]
    if Path(main).name == "cpu-homebrew.elf":
        error = ("invalid_arguments", "--sce-module requires SCE input")
    elif not module.exists():
        error = ("input_unavailable", str(module))
    elif os.environ.get("CLI_FIXTURE_REJECTION"):
        error = ("loader_failure", "SCE module graph: missing DT_NEEDED provider " + os.environ["CLI_FIXTURE_REJECTION"])
    else:
        expected = 3366582378 if profile == "sce" else 347
        status = 0 if int(a[-1]) == expected else 77
if error:
    status = 126
    records = [dict(schema_version=1, event="error", code=error[0], message=error[1], process_exit=126)]
else:
    records = [dict(schema_version=1, event="startup", executable=main, entry=16782604,
                    format="sce_elf64_x86_64", host_architecture="arm64", guest_architecture="x86_64",
                    backend="Modern QEMU TCG x86-64 dynamic translation"),
               dict(schema_version=1, event="guest_exit", exit_code=status)]
if mutation == "schema":
    records[0]["schema_version"] = 0
elif mutation == "missing_startup":
    records = records[1:]
elif mutation == "missing_exit":
    records = records[:1]
elif mutation == "wrong_startup":
    records[0]["executable"] = "wrong-executable"
elif mutation == "wrong_backend":
    records[0]["backend"] = "wrong-backend"
elif mutation == "wrong_exit":
    records[-1]["exit_code"] = 99
elif mutation == "error":
    records = [dict(schema_version=1, event="error", code="loader_failure", message="unexpected", process_exit=126)]
elif mutation == "status":
    status = 13
if mutation == "stdout":
    print("unexpected native stdout")
if os.environ.get("CLI_FIXTURE_BANNERS", "1") == "1":
    for line in {banners!r}:
        print(line + (" bad suffix" if mutation == "banner_suffix" else ""), file=sys.stderr)
for record in records:
    print(json.dumps(record), file=sys.stderr)
if mutation == "malformed":
    print('{{"schema_version":1,"event":', file=sys.stderr)
elif mutation == "teardown":
    print("fatal native teardown error", file=sys.stderr)
sys.exit(status)
'''


class RetainedCliScriptContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="CLI diagnostics independent fixture é ")
        cls.root = Path(cls.temporary.name)
        cls.runner = cls.root / "anyps5_cpu_run"
        # Only these named placeholders are copied by the complete caller
        # scripts; no executable format or native execution is simulated.
        for name in ("sce-module-main.elf", "SceModuleGuest.prx", "cpu-homebrew.elf",
                     "platform-service-main.elf", "PlatformServiceGuest.prx"):
            (cls.root / name).write_bytes(b"independent CLI transport fixture\n")
        cls.runner.write_text(RUNNER.replace("{python}", sys.executable)
                              .replace("{banners!r}", repr([API_BANNER, GPU_BANNER]))
                              .replace("{{", "{"))
        cls.runner.chmod(0o755)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_script(self, profile, mutation="", rejection=None, banners=None):
        if banners is None:
            banners = sys.platform == "darwin"
        script_root = Path(os.environ.get("ANYPS5_CLI_DIAGNOSTICS_SCRIPTS", str(HERE)))
        if profile == "sce":
            args = [script_root / "SceModuleCliTest.py", self.runner,
                    self.root / "sce-module-main.elf", self.root / "SceModuleGuest.prx",
                    self.root / "cpu-homebrew.elf"]
        else:
            args = [script_root / "PlatformServiceCliTest.py", self.runner,
                    self.root / "platform-service-main.elf", self.root / "PlatformServiceGuest.prx"]
            if rejection:
                args += ["--expect-rejection", rejection]
        environment = dict(os.environ, **ENABLED, CLI_FIXTURE_PROFILE=profile,
                           CLI_FIXTURE_MUTATION=mutation, CLI_FIXTURE_BANNERS="1" if banners else "0")
        environment.pop("CLI_FIXTURE_REJECTION", None)
        if rejection:
            environment["CLI_FIXTURE_REJECTION"] = rejection
        return subprocess.run([sys.executable, *map(str, args)], env=environment,
                              capture_output=True, text=True, timeout=15)

    def test_both_complete_retained_scripts_with_and_without_native_banners(self):
        for profile in ("sce", "platform"):
            for banners in ((False, True) if sys.platform == "darwin" else (False,)):
                with self.subTest(profile=profile, banners=banners):
                    result = self.run_script(profile, banners=banners)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_complete_missing_provider_rejection_contracts_are_retained(self):
        for provider in ("libSceNpManager.prx", "libSceNet.prx", "libSceCommonDialog.prx"):
            with self.subTest(provider=provider):
                result = self.run_script("platform", rejection=provider)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_corrupted_transport_and_retained_guest_contracts_are_rejected(self):
        cases = ["malformed", "schema", "missing_startup", "missing_exit", "wrong_startup",
                 "wrong_exit", "error", "status", "stdout", "teardown"]
        if sys.platform == "darwin":
            cases.append("banner_suffix")
        for profile in ("sce", "platform"):
            for mutation in cases + (["wrong_backend"] if profile == "platform" else []):
                with self.subTest(profile=profile, mutation=mutation):
                    result = self.run_script(profile, mutation=mutation)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertNotIn("SyntaxError", result.stderr)
                    self.assertNotIn("No such file or directory", result.stderr)


if __name__ == "__main__":
    unittest.main()
