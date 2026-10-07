#!/usr/bin/env python3
"""Extract source-qualified CPU host evidence without building or executing a guest.

This deliberately pinned adapter reads immutable Git objects, not a live worktree.
Hashes guard the manually audited resolver/type policies; updating the adapter
requires auditing a new source revision. It is not a general C++ ABI parser.
"""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


SOURCE_COMMIT = "042b41f4c9a03f65c853872f7f9dd189736225f2"
SOURCE_HASHES = {
    "core/cpu/src/Main.cpp": "6fa27a64f5fce13372956a58a5261427135ae5fe464dcced7504518fb03f88b4",
    "core/cpu/src/SceModules.cpp": "e005833adabf329c48f50330495b0b60c664e95ac08904250900b06e8ba63ced",
    "core/cpu/src/SceImports.cpp": "8c8eb7d72a93ecd535d645232b05fb0e265fdf6e37a2ac3b75df21bfa39afe99",
    "core/cpu/src/SceLifecycleImports.cpp": "e3587dc2162e15a41d39d91ea10f40d61e43f4c42b772aaac75c6e4cad4fdded",
    "core/cpu/src/SceMemoryImports.cpp": "f8755231b7ebce61a974c4d85e050edf5550b26ebc2285ffea5a831e78c95045",
    "core/cpu/src/SceCommonDialogImports.cpp": "694011218d5415a673cee5d46ea2bc4328a3dba6904a046785c958ea90515381",
    "core/cpu/src/SceNpLocalImports.cpp": "4b05cdd294451a50cafc080399fec929e8d65d0dd2b531bd6c4750478731003b",
    "core/cpu/src/SceNetAddressImports.cpp": "2712c0238e3586fd0ca717caeca7e00bfef0f30ad87c9e2fec7a382c0a35ff94",
    "core/cpu/src/SceAudioOut2Imports.cpp": "af243f6a58a9d63f7172b0cea1bc14e57a7f8b4545d2c9e87135b048fc8bca51",
    "core/cpu/src/SceSystemImports.cpp": "19c6000dd28a6c31cf56ac81fbedae9f1887725547c3a67ed047429fa13c60c7",
    "core/cpu/src/SceUserImports.cpp": "9d4d3af82213185d348aed01df011d372aef46a2828bf2b714c3b2610f635cbd",
    "core/cpu/src/SceKernelImports.cpp": "e0c0e70fb2ccecc16ffbc0a766a664b5f34f9414c92082f494d1c96bf3ed43ea",
    "core/cpu/src/SceLibcBootstrapImports.cpp": "de5a7a6a5dc6914455eac402aa4000a84bbc7452ef5bcd636864fafe6dea274a",
    "core/cpu/src/SceThreadImports.cpp": "48236166caa7662cad8aa389a621524540d0925b97e91bcfdcbc1c9f3658286e",
    "core/libs/nid/src/NidCompute.cpp": "4cf0f5349987b3cf060b7e0bf99400946edec0aef0175de0483b826b1db4b3ca",
}

# Scope and versions are audited Resolve guards in the hash-checked sources.
RESOLVERS = [
    ("SceThreadImports", "libkernel", "libkernel", 11),
    ("SceLibcBootstrapImports", None, None, 5),
    ("SceLifecycleImports", "libkernel", "libkernel", 1),
    ("SceMemoryImports", "libkernel", "libkernel", 11),
    ("SceCommonDialogImports", "libSceCommonDialog", "libSceCommonDialog", 1),
    ("SceNpLocalImports", "libSceNpManager", "libSceNpManager", 1),
    ("SceNetAddressImports", "libSceNet", "libSceNet", 4),
    ("SceAudioOut2Imports", "libSceAudioOut2", "libSceAudioOut", 20),
    ("SceSystemImports", "libSceSystemService", "libSceSystemService", 8),
    ("SceUserImports", "libSceUserService", "libSceUserService", 4),
    ("SceKernelImports", "libkernel", "libkernel", 6),
    ("SceImports", "libc", "libc", 6),
]
UNKNOWN_AUDIO = {
    "Advance", "ContextAttributes", "SpeakerInfo", "Latency",
    "MasteringInit", "MasteringTerm", "MasteringParam",
}
UNAVAILABLE_SYSTEM = {
    "GetStatus", "ReceiveEvent", "GetHdrToneMapLuminance", "LaunchPlayerDialog",
}


def git(root, *args):
    return subprocess.run(
        ["git", "-C", str(root), *args], check=True, capture_output=True
    ).stdout


def compute_nid(name):
    """Exact hash-checked NidCompute.cpp algorithm; library is not hashed."""
    digest = hashlib.sha1(name.encode("utf-8") + bytes.fromhex(
        "518d64a635ded8c1e6b039b1c3e55230"
    )).digest()
    return base64.b64encode(digest[:8][::-1], altchars=b"+-").decode().rstrip("=")


def evidence(path, text, needle, finding):
    offset = text.index(needle)
    return {"path": path, "line": text.count("\n", 0, offset) + 1,
            "source_sha256": SOURCE_HASHES[path], "finding": finding}


def extract(source_root, source_commit, modern_tcg):
    if source_commit != SOURCE_COMMIT:
        raise ValueError("Unaudited source commit; pinned adapter requires " + SOURCE_COMMIT)
    root = Path(source_root).resolve(strict=True)
    if git(root, "rev-parse", source_commit + "^{commit}").decode().strip() != SOURCE_COMMIT:
        raise ValueError("Source commit did not resolve to the audited immutable object")
    sources = {}
    for path, expected in SOURCE_HASHES.items():
        raw = git(root, "show", SOURCE_COMMIT + ":" + path)
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError("Audited source hash mismatch: " + path)
        sources[path] = raw.decode("utf-8")

    main_path = "core/cpu/src/Main.cpp"
    main = sources[main_path]
    declaration_text = main[main.index("std::vector<Cpu::SceHostModule> hosts{"):main.index("bool kernelPrx")]
    declaration_pattern = re.compile(
        r'\{"([^"]+)", \{"([^"]+)", 0, (\d+), (\d+)\}, '
        r'\{\{"([^"]+)", 0, (\d+)\}\}\}'
    )
    modules = []
    for match in declaration_pattern.finditer(declaration_text):
        filename, module, major, minor, library, version = match.groups()
        item = {"filename": filename, "module": module, "module_major": int(major),
                "module_minor": int(minor), "libraries": [
                    {"name": library, "version": int(version)}],
                "evidence": [evidence(main_path, main, match.group(),
                                       "HostModules declaration only; per-symbol resolver support is separate")]}
        if module == "libkernel":
            item["filename_aliases"] = ["libkernel.prx", "libkernel.sprx"]
        modules.append(item)
    if len(modules) != 9:
        raise ValueError("Pinned host declaration extraction count differs from audited nine")

    exports = []
    for rank, (resolver, library, module, count) in enumerate(RESOLVERS):
        path = "core/cpu/src/" + resolver + ".cpp"
        text = sources[path]
        computed = "services.emplace(Nid::ComputeNid(name," in text
        if "services{" in text:
            start = text.index("services{")
            mapping = text[start:text.index("};", start)]
        elif computed:
            start = text.index("for (const auto& [name, service]")
            mapping = text[start:text.index("services.emplace", start)]
        else:
            mapping = ""
        pairs = re.findall(r'\{"([^"]+)", Service::(\w+)\}', mapping)
        if resolver == "SceCommonDialogImports":
            pairs = [("uoUpLGNkygk", "Initialize")]
        elif resolver == "SceNpLocalImports":
            pairs = [("eQH7nWPcAgc", "GetState")]
        elif resolver == "SceLifecycleImports":
            pairs = [("6Z83sYWFlA8", "Exit")]
        if len(pairs) != count:
            raise ValueError("Pinned resolver extraction count differs: " + resolver)
        for symbol, service in pairs:
            symbol_type, size = 2, 0
            item_library, item_module = library, module
            if resolver == "SceLibcBootstrapImports":
                trace = service == "TraceInfo"
                item_library = "libSceLibcInternalExt" if trace else "libkernel"
                item_module = "libSceLibcInternal" if trace else "libkernel"
                if service in {"StackGuard", "ProgramName"}:
                    symbol_type, size = 1, 8
            qualification, reason = "qualified", "Exact audited scope/version and typed gate; bounded implementation only"
            if resolver == "SceAudioOut2Imports" and service in UNKNOWN_AUDIO:
                qualification, reason = "unknown", "Resolver gate rejects invocation: service semantics not established"
            elif resolver == "SceSystemImports" and service == "InitializePlayerDialogParam":
                qualification, reason = "unknown", "Resolver gate rejects invocation: dialog initializer unsupported"
            elif resolver == "SceThreadImports" and modern_tcg != "enabled":
                qualification, reason = "unknown", "Conditional resolver requires ANYPS5_CPU_MODERN_TCG=1; supplied profile is " + modern_tcg
            elif resolver == "SceSystemImports" and service in UNAVAILABLE_SYSTEM:
                reason = "Recognized qualified gate returns signed unavailable error without touching outputs"
            needle = '"' + symbol + '"'
            item = {"nid": compute_nid(symbol) if computed else symbol,
                    "library": item_library, "library_version": 1,
                    "module": item_module, "module_major": 1, "module_minor": 1,
                    "type": symbol_type, "size": size, "qualification": qualification,
                    "qualification_reason": reason, "resolver": resolver, "service": service,
                    "active": True,
                    "resolver_order": rank, "evidence": [
                        evidence(path, text, needle, "Actual CPU resolver service registration and scoped implementation"),
                        evidence(path, text, "import.LibraryVersion != 1", "Audited Resolve guard requires library v1 and module 1.1"),
                        evidence(main_path, main, "const std::uint8_t expectedType", "Static graph wrapper enforces bootstrap object/function type and 8-byte objects"),
                        evidence(main_path, main, "if (type != 2) return", "Static graph fallback enforces function type 2")],
                    "scope_ids": "Consumer-local library/module IDs preserved in gate key; no fixed ID requirement"}
            if computed:
                item["symbol_name"] = symbol
                nid_path = "core/libs/nid/src/NidCompute.cpp"
                item["evidence"].append(evidence(nid_path, sources[nid_path], "ComputeNid(", "Audited function-name NID derivation"))
            if resolver == "SceThreadImports":
                item["requires"] = {"ANYPS5_CPU_MODERN_TCG": True}
                item["active"] = {"enabled": True, "disabled": False, "unknown": None}[modern_tcg]
            exports.append(item)

    return {
        "schema_version": 1,
        "source": {"commit": SOURCE_COMMIT, "repository": str(root),
                   "tree": git(root, "rev-parse", SOURCE_COMMIT + "^{tree}").decode().strip(),
                   "files_sha256": SOURCE_HASHES, "read_method": "git show immutable commit:path",
                   "adapter": "pinned audited source; fail closed on source/hash/count drift"},
        "profile": {"modern_tcg": modern_tcg, "evidence_kind": "explicit analysis input; no binary profile verification"},
        "modules": modules, "exports": exports,
        "resolver_inventory_complete": True,
        "resolver_inventory_completeness": "All Main-wired resolver registrations in pinned source; conditional thread activation is explicit",
        "effective_host_policy": {
            "suppress_matching_guest_modules": True,
            "kernel_filename_alias": True,
            "suppress_for_supplied_guest_module_version": True,
            "suppression_includes_main": False,
            "kernel_alias": {"module": "libkernel", "default_filename": "libkernel.sprx",
                             "select_prx_if_needed": True, "reject_both_needed": True},
            "matching": "Exact module/name/major/minor and library/name/version; NID and type for each import",
            "guest_host_ambiguity_rejected": True,
            "named_module_provider_count_required": 1,
            "host_tls_supported": False,
            "evidence": [evidence(main_path, main, "std::erase_if(hosts", "Exact dependency guest exported module/version removes host declaration"),
                         evidence(main_path, main, "if (kernelPrx && kernelSprx)", "Both kernel filename aliases in dependency graph are rejected"),
                         evidence("core/cpu/src/SceModules.cpp", sources["core/cpu/src/SceModules.cpp"], "bool declares(", "Host scope qualification requires declared exact module and library versions")],
        },
        "unknowns": [
            "Source qualification is static registration/type/scope acceptance; not full title ABI behavior or runtime compatibility proof.",
            "Package build/profile and runtime mapped storage are not verified by this extraction.",
            "Declared host filename/module alone does not qualify any unlisted symbol.",
            "Eight registered fault-only service contracts remain unknown; thread entries are conditional on modern TCG.",
            "VideoOut source exists but is not instantiated or wired by this Main; platform-native legacy PRX exports are excluded.",
            "Missing real guest providers have unknown own dependency closure; no filenames or ABI contracts are inferred.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--modern-tcg", choices=("enabled", "disabled", "unknown"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = extract(args.source_root, args.source_commit, args.modern_tcg)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(2, "CPU host evidence extraction failed: " + str(error) + "\n")
    output = args.output.resolve()
    repository = args.source_root.resolve()
    if output.is_relative_to(repository):
        parser.exit(2, "Evidence output must be private and outside the source repository\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(json.dumps(result, indent=2, sort_keys=True) + "\n")
    except OSError as error:
        parser.exit(2, "CPU host evidence output must be a new writable file: " + str(error) + "\n")
    print(json.dumps({"output": str(output), "source_commit": SOURCE_COMMIT,
                      "module_declarations": len(result["modules"]),
                      "resolver_entries": len(result["exports"])}))


if __name__ == "__main__":
    main()
