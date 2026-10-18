"""Synthetic differential cases for the core/aot -> core/aot_cpp port.

Inputs are pure data (JSON-serializable) generated here; the expected object for each input is
produced by calling the core/aot Python reference. `difftest.py` requires the C++ port to
produce exactly the same JSON. Inputs are synthetic only -- never title data.
"""
import json
import os
import random
import sys
from pathlib import Path

AOT = Path(__file__).resolve().parents[2] / "aot"
REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(AOT))


def _ensure_capstone():
    """Make the capstone Python bindings importable.

    The instruction-level modules (atomics/integer/fma/register_*) import capstone.x86 for the
    X86_OP_* operand constants. A normal CMake build vendors the bindings under
    3rdparty/capstone/bindings/python; a lane test environment exposes them through the
    $ANYPS5_DIFFTEST_PYTHONPATH or $ANYPS5_NATIVE_WORK/tools-py variables. When none is present the
    instruction-level modules are skipped and only the capstone-free modules run, so the harness
    still works with nothing but the standard library.
    """
    try:
        import capstone  # noqa: F401
        return True
    except ImportError:
        pass
    candidates = []
    configured = os.environ.get("ANYPS5_DIFFTEST_PYTHONPATH")
    if configured:
        candidates.extend(configured.split(os.pathsep))
    candidates.append(str(REPO / "3rdparty" / "capstone" / "bindings" / "python"))
    work = os.environ.get("ANYPS5_NATIVE_WORK")
    if work:
        candidates.append(str(Path(work) / "tools-py"))
    for candidate in candidates:
        if candidate and candidate not in sys.path and Path(candidate).is_dir():
            sys.path.append(candidate)
            try:
                import capstone  # noqa: F401
                return True
            except ImportError:
                sys.path.remove(candidate)
    return False


HAVE_CAPSTONE = _ensure_capstone()

import abi  # noqa: E402
import guest_regions  # noqa: E402
import optimize  # noqa: E402
import targets  # noqa: E402
import guest_provider_calls as provider  # noqa: E402

if HAVE_CAPSTONE:
    import atomics  # noqa: E402
    import calls  # noqa: E402
    import fma  # noqa: E402
    import integer  # noqa: E402
    import register_aliases  # noqa: E402
    import register_values  # noqa: E402


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


# --- calls (structural signature; composes the ported abi module) ----------------------------

def calls_inputs():
    cases = [{"op": "structural_signature", "contract": contract} for contract in CONTRACTS]
    rng = random.Random(5150)
    for _ in range(40):
        kinds = [rng.choice(["i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "ptr", "f32", "f64"])
                 for _ in range(rng.randrange(0, 20))]
        cases.append({"op": "structural_signature",
                      "contract": {"name": "c%d" % rng.randrange(1000),
                                   "result": rng.choice(["void", "i8", "u32", "i64", "ptr", "f32", "f64"]),
                                   "parameters": kinds}})
    return cases


def calls_expected(case):
    return _safe(lambda: _norm(calls.structural_signature(case["contract"])))


# --- atomics (TSO ordering policy) -----------------------------------------------------------

def atomics_inputs():
    cases = []
    for bits in [8, 16, 32, 64, 1, 128, 0]:
        for align in [None, 1, 2, 4, 8, 16, 32]:
            cases.append({"op": "ordered_load", "bits": bits, "pointer": "%p", "align": align})
            cases.append({"op": "ordered_store", "bits": bits, "value": "%v", "pointer": "%p", "align": align})
    for size in [1, 2, 4, 8, 0, 3]:
        cases.append({"op": "accumulator", "size": size})
    rng = random.Random(717)
    for _ in range(40):
        bits = rng.choice([8, 16, 32, 64, 1, 128])
        align = rng.choice([None, 1, 2, 4, 8, 16])
        pointer = "%%p%d" % rng.randrange(10)
        cases.append({"op": "ordered_load", "bits": bits, "pointer": pointer, "align": align})
        cases.append({"op": "ordered_store", "bits": bits, "value": "%%v%d" % rng.randrange(10),
                      "pointer": pointer, "align": align})
    return cases


def atomics_expected(case):
    op = case["op"]
    if op == "ordered_load":
        return _safe(lambda: atomics.ordered_load(case["bits"], case["pointer"], case["align"]))
    if op == "ordered_store":
        return _safe(lambda: atomics.ordered_store(case["bits"], case["value"], case["pointer"], case["align"]))
    return _safe(lambda: list(atomics._accumulator(case["size"])))


# --- fma (VFMADD/VFMSUB mnemonic grammar) ----------------------------------------------------

def fma_inputs():
    valid = ["vf%s%s%s%s" % (negate, operation, order, suffix)
             for negate in ["", "n"]
             for operation in ["maddsub", "msubadd", "madd", "msub"]
             for order in ["132", "213", "231"]
             for suffix in ["ss", "sd", "ps", "pd"]]
    invalid = ["vfmadd132ps", "vfmadd132px", "vmadd132ps", "vfnmaddbus132ps", "VFMADD132PS",
               "vfmadd132", "vfmadd", "vfmsubadd213pd", "vfadd132ps", "vfmadd231s", "vf", "vfn"]
    cases = [{"op": "parts", "mnemonic": mnemonic} for mnemonic in valid + invalid]
    cases += [{"op": "vector_type", "bits": bits, "element": element}
              for bits in [128, 256, 512, 64, 32] for element in ["float", "double"]]
    rng = random.Random(321)
    alphabet = "vfnmaddsub132231ssdp"
    for _ in range(40):
        cases.append({"op": "parts", "mnemonic": "".join(rng.choice(alphabet)
                                                         for _ in range(rng.randrange(0, 14)))})
    return cases


def fma_expected(case):
    if case["op"] == "vector_type":
        return _safe(lambda: fma._vector_type(case["bits"], case["element"]))
    return _safe(lambda: _norm(fma._parts(case["mnemonic"])))


# --- integer (condition-code alias table) ----------------------------------------------------

def integer_inputs():
    names = ["e", "ne", "a", "ae", "b", "be", "g", "ge", "l", "le", "s", "ns", "o", "no", "p", "np",
             "z", "nz", "c", "nc", "na", "nbe", "nb", "nae", "ng", "nge", "nl", "nle", "pe", "po",
             "bogus", ""]
    cases = [{"op": "condition", "mnemonic": prefix + name}
             for name in names for prefix in ("cmov", "set")]
    cases += [{"op": "condition", "mnemonic": mnemonic}
              for mnemonic in ["", "ab", "cmov", "setcx", "cmovnz", "setna"]]
    rng = random.Random(20261)
    for _ in range(20):
        cases.append({"op": "condition",
                      "mnemonic": "".join(rng.choice("cmovsetnzabcglp") for _ in range(rng.randrange(0, 9)))})
    return cases


def integer_expected(case):
    ins = type("Insn", (), {"mnemonic": case["mnemonic"]})()
    return _safe(lambda: integer.condition(ins))


# --- register_values -------------------------------------------------------------------------

def register_values_inputs():
    masks = [0, 1, 2, 3, 7, 8, 15, 0xffff, (1 << 40), (1 << 62) - 1, (1 << 62),
             (1 << 32) - 1, (1 << 63) - 1, int("0x5555555555555555", 16), int("0xaaaaaaaaaaaaaaaa", 16) & ((1 << 63) - 1)]
    cases = [{"op": "low_prefix", "mask": mask} for mask in masks]
    rng = random.Random(882)
    for _ in range(40):
        cases.append({"op": "low_prefix", "mask": rng.randrange(0, 1 << 63)})
    return cases


def register_values_expected(case):
    return _safe(lambda: register_values.low_prefix(case["mask"]))


# --- register_aliases ------------------------------------------------------------------------

def register_aliases_inputs():
    fixed = [[["rax", "eax"], ["rdi", "edi"]], [], [["rax", "eax", "ax"], ["rbx", "ebx"]],
             [["r8", "r8d"], ["rax", "eax"]]]
    names = ["rax", "eax", "rdi", "edi", "r8", "r8d", "rbx", "rsp", "unknown"]
    cases = [{"op": "equivalents", "state": {"register_aliases": aliases}, "name": name}
             for aliases in fixed for name in names]
    cases.append({"op": "equivalents", "state": {}, "name": "rax"})
    rng = random.Random(4321)
    pool = ["rax", "rbx", "rcx", "rdx", "eax", "ebx", "ecx", "edx", "rdi", "rsi", "r8", "r8d", "rsp", "r12"]
    for _ in range(40):
        aliases = [sorted(rng.sample(pool, rng.randrange(1, 4))) for _ in range(rng.randrange(0, 5))]
        cases.append({"op": "equivalents", "state": {"register_aliases": aliases}, "name": rng.choice(pool)})
    return cases


def register_aliases_expected(case):
    return _safe(lambda: sorted(register_aliases.equivalents(case["state"], case["name"])))


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

# The instruction-level modules import capstone.x86 (directly or through their module bodies);
# they are skipped when the bindings are unavailable so the capstone-free modules still run.
if HAVE_CAPSTONE:
    MODULE_INPUTS.update({
        "calls": calls_inputs,
        "atomics": atomics_inputs,
        "fma": fma_inputs,
        "integer": integer_inputs,
        "register_values": register_values_inputs,
        "register_aliases": register_aliases_inputs,
    })
    MODULE_EXPECTED.update({
        "calls": calls_expected,
        "atomics": atomics_expected,
        "fma": fma_expected,
        "integer": integer_expected,
        "register_values": register_values_expected,
        "register_aliases": register_aliases_expected,
    })
