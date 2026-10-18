#!/usr/bin/env python3
"""Differential test: run the core/aot Python reference and the core/aot_cpp port on the same
synthetic inputs and require identical output.

    difftest.py --binary <aot_cpp_difftest> [--module targets ...] [--bench]

Exits non-zero on the first module whose outputs differ. `--bench` additionally reports Python
and C++ compute time on the same batches (both in-process; no per-case process start).
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import reference  # noqa: E402


def run_binary(binary, module, inputs):
    payload = "".join(json.dumps(case) + "\n" for case in inputs)
    result = subprocess.run([binary, module], input=payload, text=True, capture_output=True)
    if result.returncode != 0:
        raise SystemExit("aot_cpp_difftest %s exited %d: %s" % (module, result.returncode, result.stderr))
    lines = [line for line in result.stdout.split("\n") if line != ""]
    if len(lines) != len(inputs):
        raise SystemExit("aot_cpp_difftest %s produced %d lines for %d cases"
                         % (module, len(lines), len(inputs)))
    return [json.loads(line) for line in lines]


def check(binary, module):
    inputs = reference.MODULE_INPUTS[module]()
    expected = [reference.MODULE_EXPECTED[module](case) for case in inputs]
    actual = run_binary(binary, module, inputs)
    mismatches = [index for index in range(len(inputs)) if expected[index] != actual[index]]
    if mismatches:
        for index in mismatches[:3]:
            print("MISMATCH %s case %d" % (module, index))
            print("  input:    %s" % json.dumps(inputs[index])[:400])
            print("  python:   %s" % json.dumps(expected[index])[:400])
            print("  c++:      %s" % json.dumps(actual[index])[:400])
        raise SystemExit("%s: %d/%d differential mismatches" % (module, len(mismatches), len(inputs)))
    return len(inputs)


def bench(binary, iterations=30):
    print("speed (%d in-process iterations of the same synthetic batch):" % iterations)
    results = {}
    for module in ("optimize", "abi", "guest_regions", "targets", "calls", "atomics", "fma",
                   "integer", "register_values", "register_aliases"):
        if module not in reference.MODULE_INPUTS:
            continue
        inputs = reference.MODULE_INPUTS[module]()
        start = time.perf_counter()
        for _ in range(iterations):
            for case in inputs:
                reference.MODULE_EXPECTED[module](case)
        python_seconds = time.perf_counter() - start
        payload = "".join(json.dumps(case) + "\n" for case in inputs)
        result = subprocess.run([binary, module, "--bench", str(iterations)],
                                input=payload, text=True, capture_output=True, check=True)
        cpp_seconds = json.loads(result.stdout)["microseconds"] / 1e6
        results[module] = python_seconds / cpp_seconds
        print("  %-20s python %7.4f s  c++ %7.4f s  speedup %5.1fx  (%d cases x %d)"
              % (module, python_seconds, cpp_seconds, results[module], len(inputs), iterations))
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--module", action="append", default=[])
    parser.add_argument("--bench", action="store_true")
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    modules = args.module or list(reference.MODULE_INPUTS)
    total = 0
    for module in modules:
        total += check(binary, module)
        print("ok %s" % module)
    print("%d differential cases identical across %d modules" % (total, len(modules)))
    if args.bench:
        bench(binary)


if __name__ == "__main__":
    main()
