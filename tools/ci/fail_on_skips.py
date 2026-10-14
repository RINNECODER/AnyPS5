#!/usr/bin/env python3
"""Fail a CI job when ctest skipped any test.

Tests with SKIP_RETURN_CODE 77 (for example anyps5_metal_presentation_replay) skip when the
runner has no console session or the screen is locked. ctest reports that as Skipped, which does
not fail the job, so a regression in what the test covers can hide behind it. On the macOS CI
runner a skip is a failure. Local developer runs keep the CMake skip semantics: only the CI step
calls this script.

Usage: fail_on_skips.py ctest.xml   (the file written by ctest --output-junit)
"""
import sys
import xml.etree.ElementTree as ET


def skipped_tests(path):
    return sorted(
        case.get("name") or "?"
        for case in ET.parse(path).getroot().iter("testcase")
        if case.find("skipped") is not None
    )


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    names = skipped_tests(argv[1])
    if not names:
        print("no test skipped")
        return 0
    print(
        f"::error title=Tests skipped::{len(names)} test(s) skipped (no console session / screen locked): "
        f"{', '.join(names)}. Unlock the runner's screen and re-run."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
