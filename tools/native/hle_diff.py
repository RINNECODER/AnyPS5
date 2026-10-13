#!/usr/bin/env python3
"""Align two ANYPS5_HLE_LOG files and report the first divergence.

Both engines write the same records: a 2-word header, then per call a
`(sequence, thread, name_length, flags, args[6], result, name)` record and per
return a record with `name_length == 0` and `flags == 1` sharing the call's
sequence. The tool groups records by thread, pairs threads by their call-name
sequence, and reports the first call whose name, args or return differs. Args
that are handles/pointers can be ignored with --ignore-pointer-args.

usage: hle_diff.py <left.log> <right.log> [--ignore-pointer-args] [--limit N]
"""
import argparse
import struct
import sys
from collections import defaultdict
from pathlib import Path

MAGIC = 0x474F4C48
HEADER = 8
RECORD = 80


def read_log(path):
    data = Path(path).read_bytes()
    if len(data) < HEADER:
        raise SystemExit(f"{path}: too short")
    magic, version = struct.unpack_from("<II", data, 0)
    if magic != MAGIC:
        raise SystemExit(f"{path}: bad magic {magic:#x}")
    offset = HEADER
    calls = []
    pending = {}
    while offset + RECORD <= len(data):
        sequence, thread, name_length, flags = struct.unpack_from("<QQII", data, offset)
        args = struct.unpack_from("<6Q", data, offset + 24)
        result, = struct.unpack_from("<Q", data, offset + 72)
        offset += RECORD
        name = None
        if name_length:
            name = data[offset:offset + name_length].decode("utf-8", "replace")
            offset += name_length + ((8 - name_length % 8) % 8)
        if flags == 0:
            call = {"sequence": sequence, "thread": thread, "name": name, "args": args, "result": None}
            calls.append(call)
            pending[sequence] = call
        else:
            call = pending.get(sequence)
            if call is not None:
                call["result"] = result
    return calls


def by_thread(calls):
    threads = defaultdict(list)
    for call in calls:
        threads[call["thread"]].append(call)
    return threads


def normalized_args(args, ignore_pointers):
    if not ignore_pointers:
        return args
    # Pointers/handles: ignore values above the low handle range.
    return tuple(0 if value > 0x10000 else value for value in args)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("--ignore-pointer-args", action="store_true")
    parser.add_argument("--limit", type=int, default=10)
    args = parser.parse_args()
    left, right = read_log(args.left), read_log(args.right)
    lt, rt = by_thread(left), by_thread(right)
    left_threads = sorted(lt.items(), key=lambda item: (len(item[1]), [call["name"] for call in item[1]]))
    right_threads = sorted(rt.items(), key=lambda item: (len(item[1]), [call["name"] for call in item[1]]))
    print(f"left: {len(left)} calls, {len(lt)} threads; right: {len(right)} calls, {len(rt)} threads")
    divergences = []
    for index, ((left_id, left_calls), (right_id, right_calls)) in enumerate(zip(left_threads, right_threads)):
        for position, (left_call, right_call) in enumerate(zip(left_calls, right_calls)):
            if left_call["name"] != right_call["name"]:
                divergences.append((index, position, "name", left_call, right_call))
                break
            if normalized_args(left_call["args"], args.ignore_pointer_args) != normalized_args(right_call["args"], args.ignore_pointer_args):
                divergences.append((index, position, "args", left_call, right_call))
                break
            if left_call["result"] != right_call["result"]:
                divergences.append((index, position, "return", left_call, right_call))
                break
        else:
            if len(left_calls) != len(right_calls):
                divergences.append((index, min(len(left_calls), len(right_calls)), "length", None, None))
    for index, position, kind, left_call, right_call in divergences[:args.limit]:
        if kind == "length":
            print(f"thread pair {index}: call count {len(left_threads[index][1])} vs {len(right_threads[index][1])}")
            continue
        print(f"thread pair {index} call {position}: first differing {kind}")
        print(f"  left : {left_call['name']} args={[hex(v) for v in left_call['args']]} result={left_call['result']}")
        print(f"  right: {right_call['name']} args={[hex(v) for v in right_call['args']]} result={right_call['result']}")
    print(f"{len(divergences)} diverging thread pairs out of {min(len(left_threads), len(right_threads))}")
    return 1 if divergences else 0


if __name__ == "__main__":
    sys.exit(main())
