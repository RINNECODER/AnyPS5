#!/usr/bin/env python3
"""Check that every bind between the x86_64 libs/*.prx resolves.

dyld refuses to load a library whose import from another .prx names a symbol that
library does not export (for example a host name the NID patcher renamed). This
checks every non-weak import "from <name>.prx" against that library's export trie,
so the failure shows up at build time instead of when a title starts.

usage: check_lib_binds.py <libs-dir>
"""

import re
import subprocess
import sys
from pathlib import Path

IMPORT = re.compile(r"^\s+0x[0-9a-fA-F]+\s+(\S+)\s+(\[weak-import\]\s+)?\(from (\S+?)\)")
EXPORT = re.compile(r"^\s+0x[0-9a-fA-F]+\s+(\S+)")


def dyld_info(option, path):
    return subprocess.run(["dyld_info", option, str(path)], check=True, capture_output=True, text=True).stdout.splitlines()


def main():
    libs = Path(sys.argv[1])
    images = sorted(libs.glob("*.prx"))
    if not images:
        raise SystemExit(f"no .prx files in {libs}")
    exports = {}
    missing = []
    for image in images:
        for line in dyld_info("-imports", image):
            match = IMPORT.match(line)
            if not match or match.group(2):
                continue
            symbol, source = match.group(1), match.group(3)
            if not source.endswith(".prx"):
                continue
            if source not in exports:
                target = libs / source
                if not target.exists():
                    missing.append(f"{image.name}: {symbol} from {source} (library not built)")
                    exports[source] = set()
                    continue
                exports[source] = {m.group(1) for m in map(EXPORT.match, dyld_info("-exports", target)) if m}
            if symbol not in exports[source]:
                missing.append(f"{image.name}: {symbol} from {source}")
    for line in missing:
        print(f"unresolved bind: {line}")
    print(f"{len(images)} libraries checked, {len(missing)} unresolved binds")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
