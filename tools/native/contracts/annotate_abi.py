#!/usr/bin/env python3
"""Derive per-function and per-import ABI annotations for an AOT recovery.

Evidence, in order of strength:

  * ``verified`` — import symbols matched by NID to an HLE ``APS5_VABI`` C++ signature
    under ``--hle-root`` (the NID -> name map comes from a NID database or from the HLE
    names themselves); import thunks inherit the ABI of the import they jump to.
  * ``inferred`` — internal functions with no signature evidence get a register-layout
    ABI derived from their direct call sites (which argument registers callers populate,
    whether the return register is read). Types are placeholders: integer arguments are
    ``ptr`` and float arguments ``f64``; only the register layout is evidence-backed.
  * ``unknown`` — no evidence at all.

Nothing is asserted as verified without a signature source. Output is a JSON annotations
file consumed by ``generate_contracts.py --abi``.

usage: annotate_abi.py --elf ELF --starts STARTS.txt --hle-root core/libs/prx
       --callsites callsites.json [--nid-db aerolib.csv] --output abi.json
"""
import argparse
import glob
import json
import os
import re
import struct
import sys
from bisect import bisect_right
from collections import deque

PT_LOAD = 1
INT_ARG_ORDER = ["rdi", "rsi", "rdx", "rcx", "r8", "r9"]
FLOAT_ARG_ORDER = ["xmm%d" % i for i in range(8)]
REG64 = {}
for _r in INT_ARG_ORDER:
    REG64[_r] = _r
for _a, _b in (("rdi", "edi"), ("rsi", "esi"), ("rdx", "edx"), ("rcx", "ecx"),
               ("r8", "r8d"), ("r9", "r9d")):
    REG64[_b] = _a
for _a, _b in (("rdi", "di"), ("rsi", "si"), ("rdx", "dx"), ("rcx", "cx"),
               ("r8", "r8w"), ("r9", "r9w")):
    REG64[_b] = _a
RETURN_REG = {"rax", "eax", "ax", "al", "xmm0"}


# ---------------------------------------------------------------- HLE signatures
def find_params(text, start):
    depth, i = 1, start
    while i < len(text) and depth:
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
        i += 1
    return text[start:i - 1]


def split_params(text):
    parts, depth, current = [], 0, ""
    for ch in text:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += ch
    if current.strip():
        parts.append(current)
    return [p.strip() for p in parts]


def map_type(text):
    t = re.sub(r"\b(const|volatile|struct|class|typename|register)\b", " ", text)
    t = t.replace("std::", "").strip()
    if not t or "..." in t:
        return None
    if "(*" in t or t.endswith("*") or "*" in t:
        return "ptr"
    if t.endswith("&"):
        return "ptr"
    if t.endswith("]"):
        return "ptr"
    unsigned = bool(re.search(r"\bunsigned\b", t))
    if re.search(r"\bvoid\b", t):
        return "void"
    if re.search(r"\bbool\b", t):
        return "u8"
    if re.search(r"\bchar\b", t):
        return "u8" if unsigned else "i8"
    if re.search(r"\bshort\b", t):
        return "u16" if unsigned else "i16"
    if re.search(r"\b(float)\b", t):
        return "f32"
    if re.search(r"\b(double)\b", t):
        return "f64"
    if re.search(r"\b(u?int8_t)\b", t):
        return "u8" if unsigned else "i8"
    if re.search(r"\b(u?int16_t)\b", t):
        return "u16" if unsigned else "i16"
    if re.search(r"\b(u?int64_t|u?intptr_t|size_t|ptrdiff_t|off_t|time_t|suseconds_t|long long)\b", t):
        return "u64" if unsigned else "i64"
    if re.search(r"\blong\b", t):
        return "u64" if unsigned else "i64"
    if re.search(r"\b(u?int32_t|u?int)\b", t) or re.search(r"\benum\b", t):
        return "u32" if unsigned else "i32"
    return None


def signature_abi(ret, params_text):
    result = map_type(ret)
    if result is None:
        return None
    params = split_params(params_text)
    if len(params) == 1 and params[0].strip() in ("void", ""):
        params = []
    out = []
    for p in params:
        kind = map_type(p)
        if kind is None or kind == "void":
            return None
        out.append(kind)
    return result, out


def parse_hle(root):
    sigs = {}
    pattern = re.compile(r"([A-Za-z_][\w:<>,\*&\s]*?)\s+APS5_VABI\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    for path in glob.glob(os.path.join(root, "**", "*.cpp"), recursive=True):
        text = open(path, errors="replace").read()
        for m in pattern.finditer(text):
            ret, name = m.group(1).strip(), m.group(2)
            base = name[:-len("_nid_postfix")] if name.endswith("_nid_postfix") else name
            sigs.setdefault(base, []).append((ret, find_params(text, m.end()), path))
    return sigs


# ---------------------------------------------------------------- ELF / starts
def load_elf(path):
    raw = open(path, "rb").read()
    e_phoff = struct.unpack_from("<Q", raw, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", raw, 54)
    phdrs = []
    for i in range(e_phnum):
        t, fl, off, va, _pa, fsz, msz, al = struct.unpack_from(
            "<IIQQQQQQ", raw, e_phoff + i * e_phentsize)
        phdrs.append(dict(type=t, flags=fl, offset=off, vaddr=va, filesz=fsz, memsz=msz))
    return raw, phdrs


def translate(phdrs, addr, size=1):
    for p in phdrs:
        if p["type"] == PT_LOAD and p["vaddr"] <= addr and addr - p["vaddr"] + size <= p["filesz"]:
            return p["offset"] + addr - p["vaddr"]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True)
    ap.add_argument("--starts", required=True)
    ap.add_argument("--hle-root", required=True)
    ap.add_argument("--callsites", required=True)
    ap.add_argument("--nid-db")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    raw, phdrs = load_elf(args.elf)
    code = [(p["vaddr"], p["vaddr"] + p["filesz"]) for p in phdrs
            if p["type"] == PT_LOAD and p["flags"] & 1]
    code_lo, code_hi = min(a for a, _ in code), max(b for _, b in code)
    starts = sorted(int(l.split()[0], 16) for l in open(args.starts) if l.split())

    hle = parse_hle(args.hle_root)
    # NID fallback: compute each HLE name's NID so imports missing from the name database
    # can still be matched to a signature.
    try:
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
        sys.path.insert(0, os.path.join(root, "tools"))
        from nid_names import compute_nid
        hle_by_nid = {}
        for base in hle:
            hle_by_nid.setdefault(compute_nid(base), base)
    except Exception:
        hle_by_nid = {}
    nid_db = {}
    if args.nid_db and os.path.exists(args.nid_db):
        for line in open(args.nid_db, errors="replace"):
            parts = line.split()
            if len(parts) >= 2:
                nid_db.setdefault(parts[0], parts[1])

    # imports come from the ELF dynamic table
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from generate_contracts import parse_dynamic
    _ptrs, imports, _data = parse_dynamic(raw, phdrs)

    import_abi = {}
    for name in imports:
        nid = name.split("#")[0]
        hle_name = nid_db.get(nid)
        record = {"nid": nid, "hle_name": hle_name, "status": "unknown"}
        candidates = hle.get(hle_name, []) if hle_name else []
        if not candidates and nid in hle_by_nid:
            hle_name = hle_by_nid[nid]
            record["hle_name"] = hle_name
            record["name_source"] = "computed NID"
            candidates = hle.get(hle_name, [])
        if candidates:
            parsed = None
            for ret, params, path in candidates:
                abi = signature_abi(ret, params)
                if abi:
                    parsed = (abi, path)
                    break
            if parsed:
                (result, parameters), path = parsed
                record.update({"status": "verified", "result": result, "parameters": parameters,
                               "guest_calling_convention": "x86_64-sysv",
                               "source": "HLE APS5_VABI signature %s" % path})
            else:
                record["reason"] = "no representable HLE APS5_VABI signature for %s" % hle_name
        else:
            record["reason"] = "NID not in the name database and not an HLE name"
        import_abi[name] = record

    # import thunks: a function whose body contains a plt_import jump inherits the ABI
    function_abi = {}
    thunk_sites = {}
    for site in json.load(open(args.callsites))["sites"]:
        if site["cls"] in ("plt_import", "import_ptr") and site["form"] == "jmp_mem":
            thunk_sites[int(site["addr"], 16)] = site["detail"].get("sym")
    if thunk_sites:
        idx = 0
        for addr in sorted(thunk_sites):
            while idx + 1 < len(starts) and starts[idx + 1] <= addr:
                idx += 1
            owner = starts[idx] if starts[idx] <= addr < (starts[idx + 1] if idx + 1 < len(starts) else code_hi) else None
            if owner is None:
                continue
            sym = thunk_sites[addr]
            record = import_abi.get(sym)
            if record and record.get("status") == "verified":
                function_abi[owner] = {"status": "verified", "result": record["result"],
                                       "parameters": record["parameters"],
                                       "guest_calling_convention": "x86_64-sysv",
                                       "source": "import thunk -> %s" % sym,
                                       "import": sym}
            else:
                function_abi.setdefault(owner, {"status": "unknown",
                                                "reason": "import thunk with unverified import %s" % sym})

    # call-site register evidence -> inferred register-layout ABI
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from capstone.x86 import X86_OP_REG, X86_OP_IMM
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    code_off = translate(phdrs, code_lo, code_hi - code_lo)
    code_bytes = raw[code_off:code_off + (code_hi - code_lo)]

    def written_arg_regs(insn):
        names = set()
        for reg in insn.regs_access()[1]:
            names.add(insn.reg_name(reg))
        for op in insn.operands:
            if op.type == X86_OP_REG and op.access & 2:
                names.add(insn.reg_name(op.reg))
        return names

    def reads_return(insn):
        for reg in insn.regs_access()[0]:
            if insn.reg_name(reg) in RETURN_REG:
                return True
        for op in insn.operands:
            if op.type == X86_OP_REG and op.access & 1 and insn.reg_name(op.reg) in RETURN_REG:
                return True
        return False

    calls = {}
    window = deque(maxlen=14)
    pending = []
    addr, off = code_lo, 0
    while off < len(code_bytes):
        decoded = False
        for insn in md.disasm(code_bytes[off:off + 15], addr):
            decoded = True
            if pending and reads_return(insn):
                for target in pending:
                    calls[target]["return_used"] = True
                pending = []
            elif pending:
                pending = []
            if insn.mnemonic == "call":
                for op in insn.operands:
                    if op.type == X86_OP_IMM:
                        target = op.imm & 0xFFFFFFFFFFFFFFFF
                        if code_lo <= target < code_hi:
                            rec = calls.setdefault(target, {"call_sites": 0, "int_args": set(),
                                                            "float_args": set(), "return_used": False})
                            rec["call_sites"] += 1
                            for prev in window:
                                for name in prev:
                                    if name in REG64:
                                        rec["int_args"].add(REG64[name])
                                    elif name in FLOAT_ARG_ORDER:
                                        rec["float_args"].add(name)
                            pending.append(target)
            window.append(written_arg_regs(insn))
            off += insn.size
            addr += insn.size
        if not decoded:
            off += 1
            addr += 1

    for target, rec in calls.items():
        if target in function_abi:
            continue
        int_args = [r for r in INT_ARG_ORDER if r in rec["int_args"]]
        float_args = [r for r in FLOAT_ARG_ORDER if r in rec["float_args"]]
        parameters = ["ptr"] * len(int_args) + ["f64"] * len(float_args)
        function_abi[target] = {
            "status": "inferred",
            "result": "i64" if rec["return_used"] else "void",
            "parameters": parameters,
            "guest_calling_convention": "x86_64-sysv",
            "source": "direct call-site register evidence",
            "call_site_evidence": {"call_sites": rec["call_sites"],
                                   "integer_arg_registers": int_args,
                                   "float_arg_registers": float_args,
                                   "return_used": rec["return_used"]},
        }

    counts = {"verified": 0, "inferred": 0, "unknown": 0}
    for record in function_abi.values():
        counts[record["status"]] += 1
    import_counts = {}
    for record in import_abi.values():
        import_counts[record["status"]] = import_counts.get(record["status"], 0) + 1

    output = {
        "schema": "anyps5.aot.abi-annotations.v1",
        "hle_root": args.hle_root,
        "functions": {("%x" % a): record for a, record in function_abi.items()},
        "imports": import_abi,
        "counts": {"functions": counts, "imports": import_counts,
                   "functions_with_call_sites": len(calls)},
    }
    json.dump(output, open(args.output, "w"), separators=(",", ":"))
    print(json.dumps(output["counts"], indent=1))


if __name__ == "__main__":
    main()
