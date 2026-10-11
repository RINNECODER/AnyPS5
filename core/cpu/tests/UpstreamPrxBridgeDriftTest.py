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

* every export SceLeafSystemImports answers natively exists upstream, is not also bridged,
  and its upstream status codes are the ones the native source carries; the manifest pins
  the upstream codes so a change forces a re-audit of the native stand-in;
* the module/library/version scopes the providers declare equal the ones the target title
  was linked with (BuildLeafSystemLibsFixture.py).

After re-auditing a deliberate upstream change, regenerate the manifest with
--update and review its diff. --seed-skipped records every still-unaccounted upstream
export of a newly added library as skipped (review the reasons it picks).
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
# module -> upstream directory, upstream sources (relative to it) and the bridge descriptor file
# (None: only SceLeafSystemImports serves the module).
LIBRARIES = {
    "libSceNpManager": ("libSceNpManager", ["Export.cpp"], "NpManager.cpp"),
    "libSceNpWebApi2": ("libSceNpWebApi2", ["Export.cpp"], "NpWebApi2.cpp"),
    "libSceNpAuth": ("libSceNpAuth", ["Export.cpp"], "NpAuth.cpp"),
    "libSceNpEntitlementAccess": ("libSceNpEntitlementAccess", ["Export.cpp"], "NpEntitlementAccess.cpp"),
    "libSceSysmodule": ("libSceSysmodule", ["Export.cpp"], "Sysmodule.cpp"),
    "libSceAcm": ("libSceAcm", ["Export.cpp"], "Acm.cpp"),
    "libSceMouse": ("libSceMouse", ["Export.cpp", "src/mouse_impl.cpp"], "Mouse.cpp"),
    "libSceConvertKeycode": ("libSceConvertKeycode", ["Export.cpp"], "ConvertKeycode.cpp"),
    "libSceNetCtl": ("libSceNetCtl", ["Export.cpp"], "NetCtl.cpp"),
    "libSceSystemGesture": ("libSceSystemGesture", ["Export.cpp"], "SystemGesture.cpp"),
    "libSceNpGameIntent": ("libSceNpGameIntent", ["Export.cpp"], "NpGameIntent.cpp"),
    "libSceIme": ("libSceIme", ["Export.cpp"], "Ime.cpp"),
    "libSceImeDialog": ("libSceImeDialog", ["Export.cpp"], None),
    "libSceMsgDialog": ("libSceMsgDialog.native", ["Export.cpp"], "MsgDialog.cpp"),
    "libSceErrorDialog": ("libSceErrorDialog", ["Export.cpp"], "ErrorDialog.cpp"),
    "libSceSigninDialog": ("libSceSigninDialog", ["Export.cpp"], "SigninDialog.cpp"),
    "libSceNpCommerce": ("libSceNpCommerce", ["Export.cpp"], "NpCommerce.cpp"),
    "libScePlayerInvitationDialog": ("libScePlayerInvitationDialog", ["libScePlayerInvitationDialog.cpp"],
                                     "PlayerInvitationDialog.cpp"),
    "libSceSaveDataDialog": ("libSceSaveDataDialog.native", ["Export.cpp"], None),
    "libSceSaveData_native": ("libSceSaveData.native", ["Export.cpp"], None),
    "libSceAppContentUtil": ("libSceAppContent", ["Export.cpp"], None),
    "libSceNpUniversalDataSystem": ("libSceNpUniversalDataSystem", [
        "src/System.cpp", "src/Handle.cpp", "src/Context.cpp", "src/Event.cpp",
        "src/EventPropertyObject.cpp", "src/EventPropertyArray.cpp"], "NpUniversalDataSystem.cpp"),
    "libSceNpTrophy2": ("libSceNpTrophy2", [
        "src/Context.cpp", "src/Handle.cpp", "src/GameInfo.cpp", "src/GroupInfo.cpp", "src/TrophyInfo.cpp"],
        "NpTrophy2.cpp"),
    "libSceRtc": ("libSceRtc", ["Export.cpp"], "Rtc.cpp"),
}
NATIVE_PROVIDER = "core/cpu/src/SceLeafSystemImports.cpp"
SCOPE_BUILDER = "core/cpu/tests/BuildLeafSystemLibsFixture.py"
SKIP_REASONS = {
    "sceSysmoduleGetModuleInfoForUnwind": "reads /proc/self/maps of the host process; served from the guest module graph by platform KernelThreadExtras",
}
FORK_PROVIDERS = ("core/cpu/src/SceNpOfflineImports.cpp", NATIVE_PROVIDER)

HEADER = re.compile(r"^[ \t]*((?:const\s+)?[\w:]+(?:\s*\*)?)\s+APS5_VABI\s+(\w+)\s*\(([^)]*)\)\s*\{", re.M)
CONSTANT = re.compile(r"constexpr\s+[\w:]+\s+(\w+)\s*=\s*(?:static_cast<[\w:]+>\(\s*)?(0x[0-9A-Fa-f]+|\d+)\s*\)?\s*;")
ROW = re.compile(r"ANYPS5_UPSTREAM_EXPORT\((\w+)((?:,[^)]*)?)\)")
NATIVE_ROW = re.compile(r'\{&(\w+), "(\w+)",')


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
        part = re.sub(r"(\w+)\s*\[\s*\w*\s*\]\s*$", r"* \1", part)
        match = re.fullmatch(r"\s*(.*?)\s*\b(\w+)\s*", part)
        if not match or not match.group(1):
            raise ValueError("Cannot parse upstream parameter: " + part)
        result.append((re.sub(r"\s*\*", "*", match.group(1)), match.group(2)))
    return result


def upstream_exports(sources):
    text = "\n".join(source.read_text() for source in sources)
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


def native_exports():
    """Exports SceLeafSystemImports answers: name -> scope variable, plus its lower-cased source."""
    text = (ROOT / NATIVE_PROVIDER).read_text()
    return {name: scope for scope, name in NATIVE_ROW.findall(text)}, text.lower()


def literal(expression):
    """Numeric value of a bare literal return as a signed 32-bit value, else None."""
    if re.fullmatch(r"-?\d+|0x[0-9a-f]+", expression):
        value = int(expression, 16 if "0x" in expression else 10)
        return value - (1 << 32) if 1 << 31 <= value < 1 << 32 else value
    return None


def scope_failures():
    """Provider scopes must equal the ones the target title was linked with."""
    failures = []
    builder = (ROOT / SCOPE_BUILDER).read_text()
    linked = {module: (library, file, int(version, 16)) for module, library, file, version in re.findall(
        r'"(\w+)": \("([\w.]+)", "([\w.]+)", (0x[0-9a-f]+)', builder)}
    declared = {}
    for module, (_, _, bridge) in LIBRARIES.items():
        if not bridge:
            continue
        match = re.search(r'Library \w+Exports\(\) \{[^{}]*?return \{"(\w+)", exports(?:, "([\w.]*)", "([\w.]*)", 1, (\d))?\}',
                          (ROOT / "core/cpu/src/upstream-prx" / bridge).read_text())
        if not match:
            failures.append(f"{module}: cannot read the bridge Library scope from {bridge}")
            continue
        name, library, file, minor = match.groups()
        declared[name] = (library or name, file or name + ".prx", 0x100 | int(minor or 1))
    for match in re.finditer(r'constexpr Scope \w+\{"(\w+)", "([\w.]+)", "([\w.]+)", 1, (\d)\}',
                             (ROOT / NATIVE_PROVIDER).read_text()):
        module, library, file, minor = match.groups()
        scope = (library, file, 0x100 | int(minor))
        if declared.setdefault(module, scope) != scope:
            failures.append(f"{module}: bridge and native scopes disagree: {declared[module]} vs {scope}")
    for module, scope in sorted(declared.items()):
        if module in linked and linked[module] != scope:
            failures.append(f"{module}: provider scope {scope} differs from the title's linked scope {linked[module]}")
    for module in sorted(set(linked) - set(declared)):
        failures.append(f"{module}: the fixture imports it but no provider declares its scope")
    return failures


def audit(update, seed):
    expected = json.loads(MANIFEST.read_text()) if MANIFEST.exists() else {}
    current, failures = {}, []
    fork = "\n".join((ROOT / provider).read_text() for provider in FORK_PROVIDERS)
    natives, native_text = native_exports()
    unknown_natives = set(natives)
    for library, (directory, sources, bridge) in LIBRARIES.items():
        exports = upstream_exports([ROOT / "core/libs/prx" / directory / source for source in sources])
        rows = bridge_rows(ROOT / "core/cpu/src/upstream-prx" / bridge) if bridge else {}
        mine = {name for name in natives if name in exports}
        unknown_natives -= mine
        skipped = dict(expected.get(library, {}).get("skipped", {}))
        if seed:
            for name in sorted(set(exports) - set(rows) - mine - set(skipped)):
                skipped[name] = SKIP_REASONS.get(name) or (
                    "NotImplemented upstream" if "NotImplemented_nid_no_patch" in exports[name]["body"]
                    else "not imported by the target title; audit before bridging")
        record = {"bridged": {}, "native": {}, "skipped": skipped}
        for name in sorted(set(rows) - set(exports)):
            failures.append(f"{library}: bridge row {name} has no upstream export")
        for name in sorted(set(skipped) - set(exports)):
            failures.append(f"{library}: skipped {name} no longer exists upstream")
        for name in sorted((set(rows) | mine) & set(skipped)):
            failures.append(f"{library}: {name} is both served and skipped")
        for name in sorted(set(rows) & mine):
            failures.append(f"{library}: {name} is both bridged and answered natively")
        for name in sorted(set(exports) - set(rows) - mine - set(skipped)):
            failures.append(f"{library}: new upstream export {name} is neither served nor skipped; audit it")
        for name in sorted(mine):
            export = exports[name]
            record["native"][name] = {"nid": nid(name), "signature": export["signature"], "returns": export["returns"]}
            for value in export["returns"]:
                number = literal(value)
                if number is not None and abs(number) > 3 and not (
                        f"0x{number & 0xffffffff:x}" in native_text or str(number) in native_text):
                    failures.append(f"{library}: native {name} does not carry upstream status code {value}")
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
                if kind in ("Opaque", "Ignored"):
                    used = re.sub(r"\(void\)\s*" + parameter + r"\s*;", "", body)
                    if kind == "Ignored":
                        used = re.sub(r"^.*\bAPS5_(?:LOG_OUT|HIT)\b.*$", "", used, flags=re.M)
                    else:
                        used = re.sub(r"!\s*" + parameter + r"\b", "", used)
                        used = re.sub(r"\b" + parameter + r"\s*[!=]=\s*nullptr", "", used)
                    if re.search(r"\b" + parameter + r"\b", used):
                        failures.append(f"{library}: {name} dereferences or stores {kind} argument {parameter}")
            if re.search(r'"' + name + r'"', fork):
                failures.append(f"{library}: bridged {name} is still served by a hand-written fork provider")
        old = expected.get(library, {})
        for section in ("bridged", "native"):
            for name in sorted(set(old.get(section, {})) | set(record[section])):
                if old.get(section, {}).get(name) != record[section].get(name):
                    failures.append(f"{library}: {section} {name} drifted from the audited manifest: "
                                    f"{old.get(section, {}).get(name)} -> {record[section].get(name)}")
        current[library] = record
    for name in sorted(unknown_natives):
        failures.append(f"SceLeafSystemImports answers {name}, which no audited upstream library exports")
    failures += scope_failures()
    if update or seed:
        MANIFEST.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n")
        failures = [failure for failure in failures if "drifted from the audited manifest" not in failure and
                    "neither served nor skipped" not in failure]
    return current, failures


if __name__ == "__main__":
    current, failures = audit("--update" in sys.argv, "--seed-skipped" in sys.argv)
    if failures:
        raise SystemExit("Upstream prx bridge drift:\n  " + "\n  ".join(failures))
    bridged = sum(len(library["bridged"]) for library in current.values())
    native = sum(len(library["native"]) for library in current.values())
    skipped = sum(len(library["skipped"]) for library in current.values())
    print(f"PASS {bridged} bridged upstream exports match the audited NIDs, signatures and status codes; "
          f"{native} native stand-ins carry the upstream status codes; {skipped} skipped exports accounted for")
