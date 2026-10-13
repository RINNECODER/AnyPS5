#!/usr/bin/env python3
"""Filter the merged function-start set with the indirect lane's n10 adjudication.

Drops starts whose adjudication decision is ``interior_label`` or ``drop``, then removes
starts whose body to the next start is entirely 00/90/CC alignment padding (iterated to a
fixed point). Writes the surviving starts and a stats JSON.

usage: filter_starts.py --elf ELF --starts STARTS.txt --adjudication n10-adjudication.tsv
       --output filtered.txt [--stats stats.json]
"""
import argparse
import json
import struct

PT_LOAD = 1
PAD_BYTES = {0x00, 0x90, 0xCC}


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
    ap.add_argument("--adjudication", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--stats")
    args = ap.parse_args()

    raw, phdrs = load_elf(args.elf)
    code = [(p["vaddr"], p["vaddr"] + p["filesz"]) for p in phdrs
            if p["type"] == PT_LOAD and p["flags"] & 1]
    code_lo, code_hi = min(a for a, _ in code), max(b for _, b in code)

    starts = {}
    for line in open(args.starts):
        parts = line.split()
        if parts:
            starts[int(parts[0], 16)] = parts[1].split(",") if len(parts) > 1 else []

    decisions = {}
    header = None
    for line in open(args.adjudication):
        parts = line.rstrip("\n").split("\t")
        if header is None:
            header = parts
            continue
        decisions[int(parts[0], 16)] = parts[header.index("decision")]

    in_code = [a for a in starts if code_lo <= a < code_hi]
    excluded_out_of_code = len(starts) - len(in_code)
    dropped_interior = [a for a in in_code if decisions.get(a) in ("interior_label", "drop")]

    def body_size(a, nxt):
        j = nxt
        while j > a:
            o = translate(phdrs, j - 1, 1)
            if o is None or raw[o] not in PAD_BYTES:
                break
            j -= 1
        return j - a

    kept = sorted(a for a in in_code if a not in set(dropped_interior))
    dropped_padding = []
    while True:
        to_drop = []
        for i, a in enumerate(kept):
            nxt = kept[i + 1] if i + 1 < len(kept) else code_hi
            if body_size(a, nxt) == 0:
                to_drop.append(a)
        if not to_drop:
            break
        dropped_padding.extend(to_drop)
        drop = set(to_drop)
        kept = [a for a in kept if a not in drop]

    with open(args.output, "w") as fh:
        for a in kept:
            fh.write("%x %s\n" % (a, ",".join(starts[a])))
    stats = {
        "input_starts": len(starts),
        "excluded_out_of_code": excluded_out_of_code,
        "dropped_interior_or_drop": len(dropped_interior),
        "dropped_padding_only": len(dropped_padding),
        "kept": len(kept),
    }
    if args.stats:
        json.dump(stats, open(args.stats, "w"), indent=1)
    print(json.dumps(stats, indent=1))


if __name__ == "__main__":
    main()
