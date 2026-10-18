#!/usr/bin/env python3
"""Generate per-function AOT recovery records (the input to upstream core/aot contracts).

For an SCE x86-64 ELF this emits a recovery-style JSON
(schema ``anyps5.aot.recovery.v1``) with, per function:

  * the start address and the body size, with trailing ``int3`` / ``nop`` / zero
    alignment padding removed;
  * the indirect-call target set for every indirect call in the body (derived from the
    vtables lane's tables + call sites), and the indirect-jump target set (derived from
    the indirect lane's jump tables);
  * pointer relocations, imports and non-executable data segments from the ELF dynamic
    table.

The script is generic tooling: it reads title-derived inputs and writes title-derived
output, but embeds no title data. Run it from the lane worktree; keep the outputs in the
title work dir.

usage:
  generate_contracts.py --elf ELF --starts STARTS.txt --vtables vtables.json \
      --callsites callsites.json --jumptables jumptables.json \
      --code-relocs code-target-relocs.json --import-relocs import-pointer-relocs.json \
      --output recovery.json [--conflicts conflicts.txt]
"""
import argparse
import hashlib
import json
import struct
from bisect import bisect_right

PT_LOAD, PT_DYNAMIC = 1, 2
R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT, R_X86_64_RELATIVE = 1, 6, 7, 8
PAD_BYTES = {0x00, 0x90, 0xCC}


def load_elf(path):
    raw = open(path, "rb").read()
    e_phoff = struct.unpack_from("<Q", raw, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", raw, 54)
    e_entry = struct.unpack_from("<Q", raw, 24)[0]
    phdrs = []
    for i in range(e_phnum):
        t, fl, off, va, _pa, fsz, msz, al = struct.unpack_from(
            "<IIQQQQQQ", raw, e_phoff + i * e_phentsize)
        phdrs.append(dict(type=t, flags=fl, offset=off, vaddr=va, filesz=fsz, memsz=msz, align=al))
    return raw, phdrs, e_entry


def translate(phdrs, addr, size=1):
    for p in phdrs:
        if p["type"] == PT_LOAD and p["vaddr"] <= addr and addr - p["vaddr"] + size <= p["filesz"]:
            return p["offset"] + addr - p["vaddr"]
    return None


def load_data_segments(phdrs):
    return [{"address": p["vaddr"], "size": p["memsz"]}
            for p in phdrs if p["type"] == PT_LOAD and not p["flags"] & 1 and p["memsz"]]


def dynamic_tags(raw, phdrs):
    dyn = next((p for p in phdrs if p["type"] == PT_DYNAMIC), None)
    if dyn is None:
        return {}
    tags = {}
    i = 0
    while True:
        tag, val = struct.unpack_from("<QQ", raw, dyn["offset"] + i * 16)
        tags.setdefault(tag, val)
        i += 1
        if tag == 0:
            break
    return tags


def parse_dynamic(raw, phdrs):
    """Return (pointer_relocations, imports, data_segments). A sample may have no PT_DYNAMIC
    (freestanding) or no relocation tables at all; that is an empty set, not an error."""
    tags = dynamic_tags(raw, phdrs)
    segments = load_data_segments(phdrs)
    if 5 not in tags or 6 not in tags or 0xa not in tags:
        return [], {}, segments
    strsz = tags[0xa]
    cands = {translate(phdrs, tags[5]), translate(phdrs, tags[6])}

    def is_strtab(off):
        blob = raw[off:off + strsz]
        if not blob or blob[0] != 0:
            return False
        printable = sum(1 for b in blob if b == 0 or 0x20 <= b < 0x7f)
        return printable / len(blob) > 0.95

    strtab_off = next(c for c in cands if is_strtab(c))
    symtab_off = next(c for c in cands if not is_strtab(c))
    syment = tags.get(0xb, 24)
    if tags.get(4) is not None:
        nchain = struct.unpack_from("<I", raw, translate(phdrs, tags[4]) + 4)[0]
    else:
        nchain = max(1, (strtab_off - symtab_off) // syment)
    symbols = []
    for k in range(nchain):
        st_name, st_info, _other, st_shndx, st_value, _size = struct.unpack_from(
            "<IBBHQQ", raw, symtab_off + k * syment)
        name = None
        if st_name:
            end = raw.index(b"\x00", strtab_off + st_name, strtab_off + strsz)
            name = raw[strtab_off + st_name:end].decode("utf-8", "replace")
        symbols.append(dict(name=name, shndx=st_shndx, value=st_value, info=st_info))

    def read_relocs(off, size, ent):
        return [struct.unpack_from("<QQq", raw, off + k * ent) for k in range(size // ent)]

    rela = read_relocs(translate(phdrs, tags[7]), tags[8], tags[9]) if 7 in tags and 8 in tags and 9 in tags else []
    jmprel = read_relocs(translate(phdrs, tags[0x17]), tags[2], 24) if tags.get(0x17) and 2 in tags else []
    pointer_relocations, imports = [], {}
    for r_offset, r_info, r_addend in rela + jmprel:
        r_type, r_sym = r_info & 0xFFFFFFFF, r_info >> 32
        if r_type == R_X86_64_RELATIVE:
            pointer_relocations.append([r_offset, r_addend & 0xFFFFFFFFFFFFFFFF])
        elif r_type in (R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT):
            sym = symbols[r_sym] if r_sym < len(symbols) else None
            if sym and sym["shndx"] == 0 and sym["name"]:
                entry = imports.setdefault(sym["name"], {"symbol_index": r_sym, "slots": []})
                entry["slots"].append(r_offset)
    return pointer_relocations, imports, load_data_segments(phdrs)


def load_starts(path):
    starts = {}
    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        starts[int(parts[0], 16)] = parts[1].split(",") if len(parts) > 1 else []
    return starts


def load_table_targets(vtables_path):
    """offset -> set(targets) across all tables, plus slot -> target and all targets."""
    tables = json.load(open(vtables_path))["tables"]
    offset_targets, slot_target, all_targets = {}, {}, set()
    for table in tables:
        start = int(table["start"], 16)
        for slot, target in zip(table["slots"], table["targets"]):
            slot_addr, target_addr = int(slot, 16), int(target, 16)
            slot_target[slot_addr] = target_addr
            all_targets.add(target_addr)
            offset_targets.setdefault(slot_addr - start, set()).add(target_addr)
    return offset_targets, slot_target, all_targets


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True)
    ap.add_argument("--starts", required=True)
    ap.add_argument("--vtables", required=True)
    ap.add_argument("--callsites", required=True)
    ap.add_argument("--jumptables", required=True)
    ap.add_argument("--code-relocs")
    ap.add_argument("--import-relocs")
    ap.add_argument("--conflicts")
    ap.add_argument("--abi")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    abi_functions, abi_imports, abi_counts = {}, {}, {}
    if args.abi:
        abi_doc = json.load(open(args.abi))
        abi_functions = {int(k, 16): v for k, v in abi_doc.get("functions", {}).items()}
        abi_imports = abi_doc.get("imports", {})
        abi_counts = abi_doc.get("counts", {})

    raw, phdrs, entry = load_elf(args.elf)
    sha256 = hashlib.sha256(raw).hexdigest()
    code_ranges = [(p["vaddr"], p["vaddr"] + p["filesz"]) for p in phdrs
                   if p["type"] == PT_LOAD and p["flags"] & 1]
    code_lo, code_hi = min(a for a, _ in code_ranges), max(b for _, b in code_ranges)
    pointer_relocations, imports, data_segments = parse_dynamic(raw, phdrs)
    cross_check = {}
    if args.code_relocs:
        lane_code = json.load(open(args.code_relocs))
        elf_code = sum(1 for _slot, target in pointer_relocations if code_lo <= target < code_hi)
        cross_check["code_target_relocations"] = {"lane": len(lane_code), "elf": elf_code,
                                                 "match": len(lane_code) == elf_code}
    if args.import_relocs:
        lane_imports = json.load(open(args.import_relocs))
        cross_check["import_pointer_relocations"] = {"lane": len(lane_imports),
                                                     "elf": sum(len(v["slots"]) for v in imports.values())}

    starts = load_starts(args.starts)
    in_code = sorted(a for a in starts if code_lo <= a < code_hi)
    excluded_out_of_code = len(starts) - len(in_code)
    starts_index = in_code

    # bodies: start -> next start, trailing alignment padding trimmed
    bodies = {}
    for i, a in enumerate(in_code):
        end = in_code[i + 1] if i + 1 < len(in_code) else code_hi
        j = end
        while j > a:
            o = translate(phdrs, j - 1, 1)
            if o is None or raw[o] not in PAD_BYTES:
                break
            j -= 1
        bodies[a] = (j, end - j)

    def owner(addr):
        i = bisect_right(starts_index, addr) - 1
        if i >= 0 and starts_index[i] <= addr < bodies[starts_index[i]][0]:
            return starts_index[i]
        return None

    offset_targets, slot_target, all_table_targets = load_table_targets(args.vtables)

    # shared target-set pool (indirect-call target sets are highly repetitive)
    target_sets, target_set_ids = [], {}

    def set_id(targets):
        key = tuple(targets)
        index = target_set_ids.get(key)
        if index is None:
            index = len(target_sets)
            target_set_ids[key] = index
            target_sets.append(list(key))
        return index

    # indirect calls
    indirect_calls = {}
    call_total = call_resolved = 0
    for site in json.load(open(args.callsites))["sites"]:
        if site["form"] not in ("call_reg", "call_mem"):
            continue
        addr = int(site["addr"], 16)
        owner_addr = owner(addr)
        if owner_addr is None:
            continue
        detail = site["detail"]
        targets, source = [], "unresolved"
        if site["form"] == "call_mem" and isinstance(detail, dict) and detail.get("base") != "rip" \
                and detail.get("index") is None and isinstance(detail.get("disp"), int):
            targets = sorted(offset_targets.get(detail["disp"], ()))
            source = "vtable-slot-offset" if targets else "unresolved"
        elif site["form"] == "call_mem" and isinstance(detail, dict) and detail.get("slot"):
            slot = int(detail["slot"], 16)
            target = slot_target.get(slot)
            targets = [target] if target is not None else []
            source = "resolved-global-slot" if targets else "unresolved"
        call_total += 1
        if targets:
            call_resolved += 1
        indirect_calls.setdefault(owner_addr, []).append(
            {"address": addr, "form": site["form"], "cls": site["cls"],
             "target_set": set_id(targets), "target_count": len(targets),
             "target_source": source})

    # indirect jumps
    indirect_jumps = {}
    jump_total = jump_resolved = 0
    for table in json.load(open(args.jumptables)):
        addr = int(table["site"], 16)
        owner_addr = owner(addr)
        if owner_addr is None:
            continue
        targets = sorted({int(t, 16) for t in (table.get("resolved_targets") or [])})
        jump_total += 1
        if targets:
            jump_resolved += 1
        indirect_jumps.setdefault(owner_addr, []).append(
            {"address": addr, "table_address": int(table["table_address"], 16) if table.get("table_address") else None,
             "target_set": set_id(targets), "target_count": len(targets),
             "table_source": table.get("table_source")})

    conflicts = {}
    if args.conflicts:
        for line in open(args.conflicts):
            parts = line.split()
            if parts:
                conflicts[int(parts[0], 16)] = parts[1]

    functions = []
    complete = 0
    padding_only = 0
    functions_with_calls = 0
    functions_calls_all_resolved = 0
    functions_with_jumps = 0
    functions_jumps_all_resolved = 0
    conflict_count = 0
    abi_verified = abi_inferred = abi_unknown = 0
    complete_verified = complete_with_inferred = 0
    for a in in_code:
        body_end, padding_tail = bodies[a]
        size = body_end - a
        calls = indirect_calls.get(a, [])
        jumps = indirect_jumps.get(a, [])
        unresolved_calls = sum(1 for c in calls if not c["target_count"])
        unresolved_jumps = sum(1 for j in jumps if not j["target_count"])
        abi = abi_functions.get(a, {"status": "unknown", "reason": "no ABI evidence"})
        abi_status = abi.get("status", "unknown")
        if abi_status == "verified":
            abi_verified += 1
        elif abi_status == "inferred":
            abi_inferred += 1
        else:
            abi_unknown += 1
        missing = []
        if size <= 0:
            padding_only += 1
            missing.append("body")
        if unresolved_calls:
            missing.append("indirect_call_targets")
        if unresolved_jumps:
            missing.append("indirect_jump_targets")
        if a in conflicts:
            conflict_count += 1
            missing.append("conflict")
        if abi_status == "unknown":
            missing.append("abi")
        elif abi_status == "inferred":
            missing.append("abi_verified")
        structural_ok = size > 0 and not unresolved_calls and not unresolved_jumps and a not in conflicts
        if structural_ok:
            complete += 1
            if abi_status == "verified":
                complete_verified += 1
            if abi_status in ("verified", "inferred"):
                complete_with_inferred += 1
        if calls:
            functions_with_calls += 1
            if not unresolved_calls:
                functions_calls_all_resolved += 1
        if jumps:
            functions_with_jumps += 1
            if not unresolved_jumps:
                functions_jumps_all_resolved += 1
        functions.append({
            "address": a, "size": size, "body_end": body_end,
            "padding_tail": padding_tail,
            "symbol": None,
            "evidence": [{"kind": "merged-start", "sources": starts[a]}],
            "abi": abi, "abi_status": abi_status,
            "indirect_calls": calls,
            "indirect_jumps": jumps,
            "missing": missing,
        })

    coverage = {
        "functions": len(in_code),
        "excluded_out_of_code_starts": excluded_out_of_code,
        "functions_with_body": len(in_code) - padding_only,
        "functions_padding_only": padding_only,
        "functions_conflict": conflict_count,
        "indirect_calls": {"total": call_total, "resolved": call_resolved,
                           "unresolved": call_total - call_resolved,
                           "functions_with_calls": functions_with_calls,
                           "functions_all_resolved": functions_calls_all_resolved},
        "indirect_jumps": {"total": jump_total, "resolved": jump_resolved,
                           "unresolved": jump_total - jump_resolved,
                           "functions_with_jumps": functions_with_jumps,
                           "functions_all_resolved": functions_jumps_all_resolved},
        "complete_structural": complete,
        "abi": {"verified": abi_verified, "inferred": abi_inferred, "unknown": abi_unknown,
                "annotations": abi_counts},
        "complete_verified_abi": complete_verified,
        "complete_with_inferred_abi": complete_with_inferred,
        "missing_abi": abi_unknown,
        "pointer_relocations": len(pointer_relocations),
        "imports": len(imports),
        "data_segments": len(data_segments),
        "cross_check": cross_check,
    }

    output = {
        "schema": "anyps5.aot.recovery.v1",
        "contract_status": "structural: starts, bodies, indirect target sets, relocations; ABI annotations are verified (HLE signature/import thunk) or inferred (call-site register layout), never guessed",
        "sha256": sha256,
        "entry": entry,
        "source": args.elf.split("/")[-1],
        "generator": "tools/native/contracts/generate_contracts.py",
        "starts_source": args.starts.split("/")[-1],
        "functions": functions,
        "target_sets": target_sets,
        "imports": {name: dict(spec, abi=abi_imports.get(name, {"status": "unknown"}))
                    for name, spec in imports.items()},
        "data": data_segments,
        "pointer_relocations": pointer_relocations,
        "coverage": coverage,
        "limitations": [
            "Function bodies are start-to-next-start ranges with trailing 00/90/CC padding trimmed; interior padding and data tables are not split out.",
            "Indirect-call target sets are closed-world unions from relocation tables; register-form and unknown-provenance sites have no target set.",
            "Verified ABI comes only from HLE APS5_VABI signatures (imports and their thunks); inferred ABI is a call-site register layout with placeholder types and must not be treated as source-verified.",
        ],
    }
    with open(args.output, "w") as fh:
        json.dump(output, fh, separators=(",", ":"))
    print(json.dumps(coverage, indent=1))


if __name__ == "__main__":
    main()
