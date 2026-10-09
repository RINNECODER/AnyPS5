#!/usr/bin/env python3
"""Write the sorted {NID, name} table the CPU runtime uses to name unresolved imports.

Names come from the HLE export definitions (APS5_VABI functions) under core/libs/prx.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nid_names import compute_nid  # noqa: E402

DEFINITION = re.compile(r"\bAPS5_VABI\s+(\w+)\s*\(")


def main(prx_root, output):
    names = {}
    for path in sorted(Path(prx_root).rglob("*.cpp")):
        for name in DEFINITION.findall(path.read_text(errors="replace")):
            names.setdefault(compute_nid(name), name)
    lines = "".join('{"%s", "%s"},\n' % (nid, names[nid]) for nid in sorted(names))
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(lines)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: generate_sce_nid_names.py <core/libs/prx> <output.inc>")
    main(*sys.argv[1:])
