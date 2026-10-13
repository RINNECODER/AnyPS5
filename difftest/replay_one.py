#!/usr/bin/env python3
"""Run one differential replay case in its own process so a crashing ARM64 dylib
or x86 runner cannot take down the orchestrator. Prints one JSON object.

usage: replay_one.py <ir.ll> <image.elf> <address-hex>
"""
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay  # noqa: E402


def main():
    ir_path, image_path, address = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3], 16)
    ir = ir_path.read_text()
    image = image_path.read_bytes()
    with tempfile.TemporaryDirectory(prefix="anyps5-replay-one-") as directory:
        outcome = replay.replay_case(Path(directory), ir, f"func_{address:x}", image, address,
                                     {name: 0 for name in replay.REGISTERS}, bytes(1 << 16))
    print(json.dumps(outcome))


if __name__ == "__main__":
    main()
