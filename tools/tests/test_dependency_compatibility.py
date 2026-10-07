"""Public CLI contract tests for qualified dependency reports.

Contract: filename/declaration presence cannot establish typed scoped support;
actual consumer/provider identity differences remain visible, and reporting never
alters its input metadata. A credible regression is a filename-only shortcut,
NID-only resolver, or output alias overwriting the evidence being inspected.
Existing workflow tests cover packaging, not dependency report classification.
Synthetic metadata crosses the real CLI boundary without a test-only seam.
"""

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "dependency_compatibility.py"
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "dependency_compatibility"


def fixture(name):
    return json.loads((FIXTURES / name).read_text())


class DependencyCompatibilityContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="dependency-report-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.consumer = fixture("consumer.json")
        self.guest = fixture("guest-provider.json")
        self.host = fixture("host-evidence.json")
        self.invocations = 0

    def invoke(self, consumers=None, host=None, output_name=None, alias=None):
        consumers = consumers if consumers is not None else [self.consumer, self.guest]
        paths = []
        for index, metadata in enumerate(consumers):
            path = self.root / f"consumer-{index}.json"
            path.write_text(json.dumps(metadata, indent=2) + "\n")
            paths.append(path)
        host_path = self.root / "host.json"
        host_path.write_text(json.dumps(host if host is not None else self.host, indent=2) + "\n")
        self.invocations += 1
        output_path = self.root / (output_name or f"report-{self.invocations}.json")
        if alias == "consumer":
            output_path = paths[0]
        elif alias == "host":
            output_path = host_path
        elif alias == "symlink":
            output_path.symlink_to(paths[0])
        inputs = [*paths, host_path]
        before = {path: path.read_bytes() for path in inputs}
        args = [sys.executable, str(SCRIPT)]
        for path in paths:
            args += ["--consumer-metadata", str(path)]
        args += ["--host-evidence", str(host_path), "--output", str(output_path)]
        result = subprocess.run(args, capture_output=True, text=True, timeout=15)
        for path, content in before.items():
            self.assertEqual(path.read_bytes(), content, f"CLI altered input {path.name}")
        return result, output_path

    def report(self, **kwargs):
        result, output = self.invoke(**kwargs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(output.is_file(), "CLI did not publish the requested report")
        return json.loads(output.read_text())

    def main_import(self, report):
        # The first input is the main consumer; each report row preserves its NID.
        rows = report["consumers"][0]["imports"]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["nid"], "fixtureFunction")
        return rows[0]

    def host_provider(self, qualification="qualified"):
        host = copy.deepcopy(self.host)
        host["modules"] = [{
            "filename": "fixture.prx", "module": "fixtureModule",
            "module_major": 1, "module_minor": 1,
            "libraries": [{"name": "fixtureLibrary", "version": 1}],
            "evidence": "Public synthetic CPU declaration",
        }]
        symbol = copy.deepcopy(self.guest["exports"][0])
        symbol.update(qualification=qualification, evidence="Public synthetic resolver ABI evidence")
        host["exports"] = [symbol]
        return host

    def test_supplied_guest_function_requires_the_complete_scope_and_type(self):
        row = self.main_import(self.report())
        self.assertEqual(row["classification"], "supplied_guest")
        # Every case preserves the NID, exposing shortcuts that compare only it.
        changes = (
            ("library", "anotherLibrary"),
            ("library_version", 2),
            ("module", "anotherModule"),
            ("module_major", 2),
            ("module_minor", 2),
            ("type", 1),
        )
        for field, value in changes:
            with self.subTest(field=field):
                guest = copy.deepcopy(self.guest)
                guest["exports"][0][field] = value
                if field == "library":
                    guest["export_libraries"][0]["name"] = value
                elif field == "library_version":
                    guest["export_libraries"][0]["version"] = value
                elif field == "module":
                    guest["export_modules"][0]["name"] = value
                elif field in ("module_major", "module_minor"):
                    guest["export_modules"][0]["version"] = (
                        guest["exports"][0]["module_major"] * 256
                        + guest["exports"][0]["module_minor"]
                    )
                row = self.main_import(self.report(consumers=[self.consumer, guest]))
                self.assertEqual(row["classification"], "mismatch")

    def test_guest_filename_and_module_declaration_do_not_supply_a_nid(self):
        for declaration_only in (False, True):
            with self.subTest(declaration_only=declaration_only):
                guest = copy.deepcopy(self.guest)
                guest["exports"] = []
                if not declaration_only:
                    guest["export_modules"] = []
                    guest["export_libraries"] = []
                row = self.main_import(self.report(consumers=[self.consumer, guest]))
                self.assertEqual(row["classification"], "missing_provider")

    def test_host_requires_proved_qualification_even_with_exact_identity(self):
        for qualification, import_type, export_type, import_size, export_size, expected in (
                ("qualified", 2, 2, 0, 0, "qualified_host"),
                ("unknown", 2, 2, 0, 0, "unknown"),
                ("qualified", 2, 1, 0, 8, "mismatch"),
                ("qualified", 1, 1, 8, 4, "mismatch"),
                ("qualified", 6, 6, 8, 8, "mismatch")):
            with self.subTest(qualification=qualification, import_type=import_type,
                              export_type=export_type, export_size=export_size):
                consumer = copy.deepcopy(self.consumer)
                consumer["imports"][0].update(type=import_type, size=import_size)
                host = self.host_provider(qualification)
                host["exports"][0].update(type=export_type, size=export_size)
                row = self.main_import(self.report(consumers=[consumer], host=host))
                self.assertEqual(row["classification"], expected)
        # Service semantics can be unknown while a recorded resolver type is
        # definitely incompatible. Guest-export type coverage cannot catch
        # this separate host qualification-order regression.
        host = self.host_provider("unknown")
        host["exports"][0]["type"] = 1
        row = self.main_import(self.report(consumers=[self.consumer], host=host))
        self.assertEqual(row["classification"], "mismatch")

    def test_static_guest_object_and_tls_identity_do_not_prove_storage(self):
        for symbol_type in (1, 6):
            with self.subTest(symbol_type=symbol_type):
                consumer = copy.deepcopy(self.consumer)
                guest = copy.deepcopy(self.guest)
                consumer["imports"][0].update(type=symbol_type, size=8)
                guest["exports"][0].update(type=symbol_type, size=8)
                row = self.main_import(self.report(consumers=[consumer, guest]))
                self.assertEqual(row["classification"], "unknown")
                # A known size violation remains a mismatch despite unknown backing.
                guest["exports"][0]["size"] = 4
                row = self.main_import(self.report(consumers=[consumer, guest]))
                self.assertEqual(row["classification"], "mismatch")

    def test_duplicate_guest_scope_cannot_become_a_unique_provider(self):
        duplicate = copy.deepcopy(self.guest)
        duplicate.update(path="/public-fixtures/duplicate.prx", sha256="c" * 64)
        self.consumer["needed_files"].append("duplicate.prx")
        row = self.main_import(self.report(consumers=[self.consumer, self.guest, duplicate]))
        self.assertEqual(row["classification"], "mismatch")

    def test_declared_host_absent_nid_respects_resolver_inventory_completeness(self):
        for complete, expected in ((False, "unknown"), (True, "missing_provider")):
            with self.subTest(complete=complete):
                host = self.host_provider()
                host["exports"] = []
                host["resolver_inventory_complete"] = complete
                row = self.main_import(self.report(consumers=[self.consumer], host=host))
                self.assertEqual(row["classification"], expected)

    def test_absent_nid_is_missing_even_when_guest_module_is_supplied(self):
        self.guest["exports"][0]["nid"] = "anotherFunction"
        row = self.main_import(self.report())
        self.assertEqual(row["classification"], "missing_provider")

    def test_all_supplied_consumers_and_absent_provider_closures_are_reported(self):
        self.consumer["needed_files"] += ["missing-a.prx", "missing-b.prx"]
        self.guest["needed_files"] = ["missing-a.prx", "transitive.prx"]
        report = self.report()
        missing = {(row["filename"], tuple(row["required_by"]))
                   for row in report["unknown_provider_closure"]}
        self.assertEqual(missing, {
            ("missing-a.prx", ("fixture.prx", "main.bin")),
            ("missing-b.prx", ("main.bin",)),
            ("transitive.prx", ("fixture.prx",)),
        })
        self.assertTrue(all(row["classification"] == "unknown"
                            for row in report["unknown_provider_closure"]))
        self.assertEqual(report["runtime_compatibility"], "not_established")
        self.assertEqual([consumer["filename"] for consumer in report["consumers"]],
                         ["main.bin", "fixture.prx"])

    def test_existing_output_is_preserved_on_refusal(self):
        output = self.root / "existing.json"
        original = b"existing private report must survive\n"
        output.write_bytes(original)
        result, returned_output = self.invoke(output_name=output.name)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(returned_output.read_bytes(), original)

    def test_ordered_cpu_resolver_fallback_is_not_a_provider_collision(self):
        # Main resolves an optional earlier adapter before its always-on fallback.
        # Counting registrations as providers invents ambiguity; existing guest
        # collision coverage does not enforce this CPU resolver-chain contract.
        for active, first_qualification, expected in (
                (True, "qualified", "qualified_host"),
                (False, "unknown", "qualified_host"),
                (None, "unknown", "unknown"),
                (None, "qualified", "unknown"),
                (True, "unknown", "unknown")):
            with self.subTest(active=active, qualification=first_qualification):
                host = self.host_provider()
                fallback = host["exports"][0]
                fallback.update(active=True, resolver_order=10)
                first = copy.deepcopy(fallback)
                first.update(active=active, resolver_order=0,
                             qualification=first_qualification)
                host["exports"].insert(0, first)
                row = self.main_import(self.report(consumers=[self.consumer], host=host))
                self.assertEqual(row["classification"], expected)

    def test_frozen_host_selection_suppresses_guest_modules_and_selects_kernel_alias(self):
        host = self.host_provider()
        host["effective_host_policy"] = {
            "suppress_matching_guest_modules": True, "kernel_filename_alias": True}
        report = self.report(host=host)
        self.assertEqual(self.main_import(report)["classification"], "supplied_guest")
        self.assertEqual(len(report["suppressed_host_modules"]), 1)
        self.assertEqual(report["effective_host_modules"], [])
        consumer = copy.deepcopy(self.consumer)
        host = self.host_provider()
        host["effective_host_policy"] = {"kernel_filename_alias": True}
        consumer["needed_files"] = ["libkernel.prx"]
        consumer["import_modules"][0]["name"] = "libkernel"
        consumer["import_libraries"][0]["name"] = "libkernel"
        consumer["imports"][0].update(module="libkernel", library="libkernel")
        host["modules"][0].update(filename="libkernel.sprx", module="libkernel")
        host["modules"][0]["libraries"][0]["name"] = "libkernel"
        host["exports"][0].update(module="libkernel", library="libkernel")
        report = self.report(consumers=[consumer], host=host)
        self.assertEqual(self.main_import(report)["classification"], "qualified_host")
        self.assertEqual(report["effective_host_modules"][0]["filename"], "libkernel.prx")
        consumer["needed_files"].append("libkernel.sprx")
        report = self.report(consumers=[consumer], host=host)
        self.assertTrue(any(issue["classification"] == "mismatch"
                            and issue["reason"] == "both_kernel_filename_aliases_requested"
                            for issue in report["graph_issues"]))

    def test_output_cannot_alias_consumer_or_host_evidence(self):
        for alias in ("consumer", "host", "symlink"):
            with self.subTest(alias=alias):
                result, _ = self.invoke(alias=alias)
                self.assertNotEqual(result.returncode, 0, "input/output alias was accepted")


if __name__ == "__main__":
    unittest.main()
