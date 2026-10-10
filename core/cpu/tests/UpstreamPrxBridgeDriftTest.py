"""Fail when upstream core/libs/prx NP exports drift away from the bridge's audit.

The bridge descriptors in core/cpu/src/upstream-prx/*.cpp were checked by hand
against the upstream bodies. This test re-reads the upstream Export.cpp files and
compares them with UpstreamPrxBridgeManifest.json, the record of that audit:

* every upstream export is either bridged or listed as skipped with a reason, so a
  new upstream export cannot appear unnoticed;
* the bridged set, its NIDs, signatures and returned status codes (constants
  resolved to their values) match the manifest;
* a bridged export never calls NotImplemented and takes no callback, and every
  argument marked Opaque is only ignored or null-checked by the upstream body;
* no bridged export is still served by a hand-written fork provider.

After re-auditing a deliberate upstream change, regenerate the manifest with
--update and review its diff.
"""

import json
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from BuildSceModulesFixture import nid  # noqa: E402

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else \
    Path(__file__).resolve().parents[3]
MANIFEST = Path(__file__).resolve().parent / "UpstreamPrxBridgeManifest.json"
LIBRARIES = {
    "libSceNpManager": "NpManager.cpp",
    "libSceNpWebApi2": "NpWebApi2.cpp",
    "libSceNpAuth": "NpAuth.cpp",
    "libSceNpEntitlementAccess": "NpEntitlementAccess.cpp",
}
FORK_PROVIDERS = ("core/cpu/src/SceNpOfflineImports.cpp",)

HEADER = re.compile(r"^[ \t]*((?:const\s+)?[\w:]+(?:\s*\*)?)\s+APS5_VABI\s+(\w+)\s*\(([^)]*)\)\s*\{", re.M)
CONSTANT = re.compile(r"constexpr\s+[\w:]+\s+(\w+)\s*=\s*(?:static_cast<[\w:]+>\(\s*)?(0x[0-9A-Fa-f]+|\d+)\s*\)?\s*;")
ROW = re.compile(r"ANYPS5_UPSTREAM_EXPORT\((\w+)((?:,[^)]*)?)\)")


def body_end(text, start):
    depth = 0
    for index in range(start, len(text)):
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        if not depth:
            return index + 1
    raise ValueError("Unbalanced upstream function body")


def parameters(text):
    text = " ".join(text.split())
    if text in ("", "void"):
        return []
    result = []
    for part in text.split(","):
        match = re.fullmatch(r"\s*(.*?)\s*\b(\w+)\s*", part)
        if not match or not match.group(1):
            raise ValueError("Cannot parse upstream parameter: " + part)
        result.append((re.sub(r"\s*\*", "*", match.group(1)), match.group(2)))
    return result


def upstream_exports(source):
    text = source.read_text()
    constants = dict(CONSTANT.findall(text))
    exports = {}
    for header in HEADER.finditer(text):
        returns, name, raw = header.groups()
        body = text[header.end() - 1:body_end(text, header.end() - 1)]
        local = {**constants, **dict(CONSTANT.findall(body))}
        resolved = []
        for expression in re.findall(r"\breturn\b\s*([^;]*);", body):
            expression = " ".join(expression.split())
            for constant, value in local.items():
                expression = re.sub(r"\b" + constant + r"\b", value.lower(), expression)
            resolved.append(expression)
        params = parameters(raw)
        exports[name] = {
            "signature": re.sub(r"\s*\*", "*", returns) + "(" + ", ".join(t + " " + n for t, n in params) + ")",
            "returns": sorted(set(resolved)),
            "params": params,
            "body": body,
        }
    return exports


def bridge_rows(path):
    rows = {}
    for name, kinds in ROW.findall(path.read_text()):
        kinds = [kind.strip() for kind in kinds.split(",") if kind.strip()]
        if name in rows:
            raise AssertionError("Duplicate bridge row " + name)
        rows[name] = kinds
    return rows


def audit(update):
    expected = json.loads(MANIFEST.read_text()) if MANIFEST.exists() else {}
    current, failures = {}, []
    fork = "\n".join((ROOT / provider).read_text() for provider in FORK_PROVIDERS)
    for library, bridge in LIBRARIES.items():
        exports = upstream_exports(ROOT / "core/libs/prx" / library / "Export.cpp")
        rows = bridge_rows(ROOT / "core/cpu/src/upstream-prx" / bridge)
        skipped = expected.get(library, {}).get("skipped", {})
        record = {"bridged": {}, "skipped": skipped}
        for name in sorted(set(rows) - set(exports)):
            failures.append(f"{library}: bridge row {name} has no upstream export")
        for name in sorted(set(skipped) - set(exports)):
            failures.append(f"{library}: skipped {name} no longer exists upstream")
        for name in sorted(set(rows) & set(skipped)):
            failures.append(f"{library}: {name} is both bridged and skipped")
        for name in sorted(set(exports) - set(rows) - set(skipped)):
            failures.append(f"{library}: new upstream export {name} is neither bridged nor skipped; audit it")
        for name, kinds in sorted(rows.items()):
            if name not in exports:
                continue
            export = exports[name]
            record["bridged"][name] = {"nid": nid(name), "signature": export["signature"], "returns": export["returns"]}
            body = export["body"]
            if "NotImplemented_nid_no_patch" in body:
                failures.append(f"{library}: bridged {name} calls NotImplemented upstream")
            if re.search(r"Register\w*Callback", name):
                failures.append(f"{library}: bridged {name} registers a callback; keep it hand-written")
            if len(kinds) != len(export["params"]):
                failures.append(f"{library}: {name} row has {len(kinds)} argument kinds for {len(export['params'])} parameters")
                continue
            for kind, (kind_type, parameter) in zip(kinds, export["params"]):
                if ("callback" in parameter.lower() and kind_type.endswith("*")) or "(*" in kind_type:
                    failures.append(f"{library}: bridged {name} takes callback parameter {parameter}")
                if kind == "Opaque":
                    used = re.sub(r"\(void\)\s*" + parameter + r"\s*;", "", body)
                    used = re.sub(r"!\s*" + parameter + r"\b", "", used)
                    used = re.sub(r"\b" + parameter + r"\s*[!=]=\s*nullptr", "", used)
                    if re.search(r"\b" + parameter + r"\b", used):
                        failures.append(f"{library}: {name} dereferences or stores Opaque argument {parameter}")
            if re.search(r'"' + name + r'"', fork):
                failures.append(f"{library}: bridged {name} is still served by a hand-written fork provider")
        old = expected.get(library, {}).get("bridged", {})
        for name in sorted(set(old) | set(record["bridged"])):
            if old.get(name) != record["bridged"].get(name):
                failures.append(f"{library}: {name} drifted from the audited manifest: "
                                f"{old.get(name)} -> {record['bridged'].get(name)}")
        current[library] = record
    if update:
        MANIFEST.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n")
        failures = [failure for failure in failures if "drifted from the audited manifest" not in failure]
    return current, failures


if __name__ == "__main__":
    current, failures = audit("--update" in sys.argv)
    if failures:
        raise SystemExit("Upstream prx bridge drift:\n  " + "\n  ".join(failures))
    bridged = sum(len(library["bridged"]) for library in current.values())
    skipped = sum(len(library["skipped"]) for library in current.values())
    print(f"PASS {bridged} bridged upstream NP exports match the audited NIDs, signatures and status codes; "
          f"{skipped} skipped exports accounted for")
