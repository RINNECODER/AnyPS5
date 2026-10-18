"""Synthetic differential cases for the core/aot -> core/aot_cpp port.

Inputs are pure data (JSON-serializable) generated here; the expected object for each input is
produced by calling the core/aot Python reference. `difftest.py` requires the C++ port to
produce exactly the same JSON. Inputs are synthetic only -- never title data.
"""
import json
import random
import sys
from pathlib import Path

AOT = Path(__file__).resolve().parents[2] / "aot"
sys.path.insert(0, str(AOT))

import abi  # noqa: E402
import guest_regions  # noqa: E402
import optimize  # noqa: E402
import targets  # noqa: E402
import guest_provider_calls as provider  # noqa: E402


def _norm(value):
    return json.loads(json.dumps(value))


def _safe(function):
    try:
        return _norm(function())
    except Exception:
        return {"error": True}


def _signature(contract):
    result, parameters = abi.signature(contract)
    return {"result": result, "parameters": list(parameters)}


def _merge(ir):
    text, count = optimize.merge_constant_stores(ir)
    return {"ir": text, "count": count}


# --- targets ---------------------------------------------------------------------------------

def targets_inputs():
    cases = [{"op": "spec", "name": name}
             for name in list(targets.TARGETS) + ["bogus", "", "apple-x86"]]
    cases.append({"op": "all"})
    return cases


def targets_expected(case):
    if case["op"] == "all":
        return _norm(targets.TARGETS)
    return _safe(lambda: targets.target_spec(case["name"]))


# --- abi -------------------------------------------------------------------------------------

KINDS = ["i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "ptr", "f32", "f64",
         "void", "i128", "f80", "bad", ""]
OPS = ["bits", "llvm_type", "extension", "parameter_type", "result_type", "return_register"]
CONTRACTS = [
    {"name": "entry", "result": "void", "parameters": []},
    {"name": "f", "result": "i32", "parameters": ["i64", "f32"]},
    {"name": "spill", "result": "f64", "parameters": ["i64"] * 9 + ["f64"] * 10},
    {"name": "uniform", "result": "void", "parameters": [],
     "guest_calling_convention": "x86_64-uniform"},
    {"name": "uniform_flag", "result": "void", "uniform": True},
    {"name": "uniform_bad", "result": "i32", "parameters": [], "uniform": True},
    {"name": "fast", "result": "i32", "parameters": ["i64", "f32"],
     "guest_calling_convention": "x86_64-llvm-fastcc"},
    {"name": "fast_ok", "result": "i32", "parameters": ["i64", "f32"],
     "guest_calling_convention": "x86_64-llvm-fastcc",
     "guest_locations": [{"kind": "register", "name": "rdi", "bits": 64, "type": "i64"},
                         {"kind": "register", "name": "xmm0", "bits": 32, "type": "f32"}],
     "guest_return_register": "rax"},
    {"name": "fast_drift", "result": "i32", "parameters": ["i64", "f32"],
     "guest_calling_convention": "x86_64-llvm-fastcc",
     "guest_locations": [{"kind": "register", "name": "rcx", "bits": 64, "type": "i64"},
                         {"kind": "register", "name": "xmm0", "bits": 32, "type": "f32"}],
     "guest_return_register": "rax"},
    {"name": "variadic", "result": "i32", "parameters": [], "variadic": True},
    {"name": "sret", "result": "i32", "parameters": [], "sret": "ptr"},
    {"name": "byval", "result": "i32", "parameters": [], "byval": ["ptr"]},
    {"name": "bad_type", "result": "i128", "parameters": []},
    {"name": "1bad", "result": "void", "parameters": []},
    {"result": "void", "parameters": []},
    {"name": "weird_cc", "result": "void", "parameters": [], "guest_calling_convention": "sparc"},
]
ANNOTATIONS = [
    ({"imports": {"nid": {"name": "provider", "result": "ptr", "parameters": ["ptr", "u64", "u64"]}}},
     {"imports": {"nid": {"status": "verified", "hle_name": "provider",
                          "result": "ptr", "parameters": ["ptr", "u64", "u64"]}}}),
    ({"imports": {"nid": {"name": "s", "result": "i64", "parameters": [], "stub": True}}},
     {"imports": {"nid": {"status": "verified", "hle_name": "s",
                          "result": "ptr", "parameters": ["ptr"]}}}),
    ({"imports": {"nid": {"name": "s", "result": "i64", "parameters": []}}}, {"imports": {}}),
    ({"imports": {"nid": {"name": "s", "result": "i64", "parameters": []}}},
     {"imports": {"nid": {"status": "verified", "hle_name": "s",
                          "result": "ptr", "parameters": ["ptr"]}}}),
    ({"imports": {"nid": {"name": "s", "result": "i64", "parameters": [], "stub": True}}},
     {"imports": {"nid": {"status": "unverified"}}}),
]


def abi_inputs():
    cases = [{"op": op, "kind": kind} for kind in KINDS for op in OPS]
    cases.append({"op": "uniform_locations"})
    cases.append({"op": "uniform_stack_end"})
    for contract in CONTRACTS:
        cases.append({"op": "uniform", "contract": contract})
        cases.append({"op": "signature", "contract": contract})
    for contracts, annotations in ANNOTATIONS:
        cases.append({"op": "validate_import_annotations",
                      "contracts": contracts, "annotations": annotations})
    rng = random.Random(20261017)
    for _ in range(40):
        kinds = [rng.choice(["i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "ptr", "f32", "f64"])
                 for _ in range(rng.randrange(0, 20))]
        contract = {"name": "r%d" % rng.randrange(1000),
                    "result": rng.choice(["void", "i8", "u32", "i64", "ptr", "f32", "f64"]),
                    "parameters": kinds}
        cases.append({"op": "locations", "parameters": kinds})
        cases.append({"op": "signature", "contract": contract})
    return cases


def abi_expected(case):
    op = case["op"]
    if op == "uniform_locations":
        return _norm(abi.uniform_locations())
    if op == "uniform_stack_end":
        return abi.uniform_stack_end()
    if op == "uniform":
        return _safe(lambda: abi.uniform(case["contract"]))
    if op == "signature":
        return _safe(lambda: _signature(case["contract"]))
    if op == "validate_import_annotations":
        return _safe(lambda: abi.validate_import_annotations(case["contracts"], case["annotations"]))
    if op == "locations":
        return _safe(lambda: abi.locations(case["parameters"]))
    return _safe(lambda: getattr(abi, op)(case["kind"]))


# --- guest_regions ---------------------------------------------------------------------------

def guest_regions_inputs():
    cases = []
    fixed = [
        ([{"vaddr": 0x1000, "memsz": 0x8000, "flags": 5}], (0x1000, 0x4000)),
        ([{"vaddr": 0x1000, "memsz": 0x8000, "flags": 4}], (0, 0)),
        ([{"vaddr": 0, "memsz": 0, "flags": 7}], (0, 0)),
        ([{"vaddr": -1, "memsz": 16, "flags": 5}], (0, 0)),
        ([{"vaddr": 0x20000, "memsz": 0x4000, "flags": 6}], (0x20000, 0x4000)),
        ([{"vaddr": 0x4000, "memsz": 0x4000 + 1, "flags": 5}], (0, 0)),
    ]
    for loads, relro in fixed:
        cases.append({"op": "immutable_ranges", "loads": loads, "relro": list(relro)})
    rng = random.Random(4242)
    for _ in range(60):
        loads = [{"vaddr": rng.randrange(0, 1 << 40) & ~0x3fff,
                  "memsz": rng.randrange(0, 1 << 20),
                  "flags": rng.randrange(0, 8)}
                 for _ in range(rng.randrange(0, 5))]
        relro = (rng.randrange(0, 1 << 40) & ~0x3fff, rng.randrange(0, 1 << 20))
        cases.append({"op": "immutable_ranges", "loads": loads, "relro": list(relro)})
    ranges = [[0x1000, 0x5000], [0x9000, 0xa000]]
    for address in [0, 0x1000, 0x2fff, 0x5000, 0x9000, 0x9fff, 0xa000, -5]:
        cases.append({"op": "available_bytes", "ranges": ranges, "address": address})
    return cases


def guest_regions_expected(case):
    if case["op"] == "available_bytes":
        return _safe(lambda: guest_regions.available_bytes([tuple(r) for r in case["ranges"]],
                                                           case["address"]))
    return _safe(lambda: [list(pair) for pair in
                          guest_regions.immutable_ranges(case["loads"], tuple(case["relro"]))])


# --- guest_provider_calls --------------------------------------------------------------------

def guest_provider_calls_inputs():
    return [{"op": "calls"}, {"op": "declarations"}]


def guest_provider_calls_expected(case):
    if case["op"] == "declarations":
        return _norm(provider.declarations())
    return _norm([[symbol, list(provider.CALLS[symbol])] for symbol in provider.CALLS])


# --- optimize --------------------------------------------------------------------------------

def optimize_inputs():
    triple = 'target triple = "arm64-apple-macosx13.0.0"\n'
    body = (triple + "define void @f() {\n"
            "  %buf = alloca [64 x i8], align 16\n"
            "  %p0 = getelementptr inbounds i8, ptr %buf, i64 0\n"
            "  store i64 1234605616436508552, ptr %p0, align 8\n"
            "  %p1 = getelementptr inbounds i8, ptr %buf, i64 8\n"
            "  store i64 1234605616436508552, ptr %p1, align 8\n"
            "  %p2 = getelementptr i8, ptr %buf, i64 4\n"
            "  store i32 7, ptr %p2, align 4\n"
            "  %small = getelementptr i8, ptr %buf, i64 48\n"
            "  store i32 1, ptr %small, align 4\n"
            "  ret void\n}\n")
    declared = body.replace("define void @f() {",
                            "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n"
                            "define void @f() {")
    edge_short = (triple + "define void @g() {\n"
                  "  %b = alloca [24 x i8], align 16\n"
                  "  %q0 = getelementptr nuw i8, ptr %b, i64 0\n"
                  "  store i8 -128, ptr %q0, align 1\n"
                  "  %q1 = getelementptr inbounds i8, ptr %b, i64 1\n"
                  "  store i16 4660, ptr %q1, align 2\n"
                  "  %q7 = getelementptr i8, ptr %b, i64 7\n"
                  "  store i8 1, ptr %q7, align 1\n"
                  "  %oob = getelementptr i8, ptr %b, i64 20\n"
                  "  store i64 9, ptr %oob, align 8\n"
                  "  %end = getelementptr i8, ptr %b, i64 24\n"
                  "  store i8 2, ptr %end, align 1\n"
                  "  ret void\n}\n")
    edge_full = (triple + "define void @h() {\n"
                 "  %s = alloca [24 x i8], align 16\n"
                 "  %s0 = getelementptr i8, ptr %s, i64 0\n"
                 "  store i64 81985529216486895, ptr %s0, align 8\n"
                 "  %s8 = getelementptr i8, ptr %s, i64 8\n"
                 "  store i64 81985529216486895 release, ptr %s8, align 8\n"
                 "  %s16 = getelementptr i8, ptr %s, i64 16\n"
                 "  store atomic i64 81985529216486895, ptr %s16 seq_cst, align 8\n"
                 "  %bad = alloca [16 x i8]\n"
                 "  %noeq = alloca [16 x i8],\n"
                 "  %trailing = getelementptr i8, ptr %s, i64 4 extra\n"
                 "  store i8 3, ptr %s, align 1\n"
                 "  ret void\n}\n")
    inputs = [
        {"ir": triple},
        {"ir": 'target triple = "x86_64-pc-linux-gnu"\n'},
        {"ir": triple + "@__aps5_aot_bytes_0 = constant [16 x i8] zeroinitializer\n"},
        {"ir": body},
        {"ir": body.replace("store i64", "store atomic i64").replace(", align", " release, align")},
        {"ir": declared},
        {"ir": edge_short},
        {"ir": edge_full},
        {"ir": ""},
        {"ir": triple},
    ]
    rng = random.Random(99)
    for _ in range(60):
        inputs.append({"ir": _random_ir(rng)})
    return inputs


def optimize_expected(case):
    return _safe(lambda: _merge(case["ir"]))


def _random_ir(rng):
    out = ['target triple = "arm64-apple-macosx13.0.0"', "", "define void @f0() {"]
    pointers = []
    for index in range(70):
        roll = rng.random()
        if roll < 0.15:
            name = f"%a{index}"
            size = rng.choice([8, 16, 32, 48, 64])
            out.append(f"  {name} = alloca [{size} x i8], align 16")
            pointers.append(name)
        elif roll < 0.35 and pointers:
            name = f"%g{index}"
            keyword = rng.choice(["", " inbounds", " nuw"])
            out.append(f"  {name} = getelementptr{keyword} i8, ptr {rng.choice(pointers)}, "
                       f"i64 {rng.randrange(-16, 64)}")
            pointers.append(name)
        elif roll < 0.72 and pointers:
            width = rng.choice([8, 16, 32, 64])
            order = rng.choice(["", " monotonic", " acquire", " release", " acq_rel", " seq_cst"])
            atomic = "atomic " if rng.random() < 0.3 else ""
            out.append(f"  store {atomic}i{width} {rng.randrange(-1 << 20, 1 << 20)}, "
                       f"ptr {rng.choice(pointers)}{order}, align {rng.choice([1, 2, 4, 8])}")
        elif roll < 0.78:
            out.append(f"  store i32 5, ptr %missing{index}, align 4")
        elif roll < 0.83:
            out.append("}")
            pointers = []
        elif roll < 0.9:
            out.append(f"  %v{index} = add i64 {rng.randrange(10)}, {rng.randrange(10)}")
        else:
            out.append(rng.choice(["  ret void", "  call void @helper()",
                                   "  %z = alloca [16 x i8]", f"label{index}:"]))
    out.append("}")
    return "\n".join(out) + "\n"


MODULE_INPUTS = {
    "targets": targets_inputs,
    "abi": abi_inputs,
    "guest_regions": guest_regions_inputs,
    "guest_provider_calls": guest_provider_calls_inputs,
    "optimize": optimize_inputs,
}

MODULE_EXPECTED = {
    "targets": targets_expected,
    "abi": abi_expected,
    "guest_regions": guest_regions_expected,
    "guest_provider_calls": guest_provider_calls_expected,
    "optimize": optimize_expected,
}
