#!/usr/bin/env python3
"""Map a pull request's changed files to the macOS Metal CI jobs they can affect.

Used by the "changes" job in .github/workflows/macos-metal.yml. Rules are ordered and the
first match wins; a path that matches no rule selects every job (fail safe).
"""
import argparse
import fnmatch
import json
import os
import subprocess
import sys

CPU, NATIVE, SHADER, REPLAY = "cpu-tcg", "native-runner", "shader-bridge", "guest-replay"
JOBS = (CPU, NATIVE, SHADER, REPLAY)
ALL = frozenset(JOBS)
NONE = frozenset()

# (glob, jobs). fnmatch's "*" also crosses "/", so "dir/*" covers a whole subtree.
RULES = (
    # Global: build system, submodules, CI itself and the packaging tools the jobs execute.
    ("CMakeLists.txt", ALL),
    ("cmake/*", ALL),
    (".gitmodules", ALL),
    ("3rdparty/*", ALL),
    (".github/*", ALL),
    ("tools/ci/*", ALL),
    ("tools/package_release.py", ALL),
    ("tools/diagnostic_package.py", ALL),
    ("tools/prepare_diagnostic_engine.py", ALL),
    ("tools/native_diagnostic_controls.cmake", ALL),
    ("tools/ctest_unobtrusive_windows.cmake", ALL),
    ("tools/release/*", ALL),
    ("tools/tests/test_native_diagnostic_package.py", ALL),
    # Nothing a macOS job builds or runs.
    ("docs/*", NONE),
    ("*.md", NONE),
    ("LICENSE", NONE),
    # Shader bridge (SPIR-V to MSL) and its replay.
    ("core/shader/recompiler/MetalReplay/*", frozenset({REPLAY})),
    ("core/shader/*", frozenset({SHADER, REPLAY})),
    # Metal/AGC execution and video out.
    ("core/libs/prx/libSceAgcDriver/Graphics/Metal/*", frozenset({NATIVE, SHADER, REPLAY})),
    ("core/libs/prx/libSceAgcDriver/Execution/Metal/*", frozenset({NATIVE, REPLAY})),
    ("core/libs/prx/libSceAgc/*", frozenset({NATIVE, REPLAY})),
    ("core/libs/prx/libSceVideoOut/*", frozenset({NATIVE, REPLAY})),
    ("core/cpu/videoout/*", frozenset({NATIVE, REPLAY})),
    ("core/cpu/*Metal*", frozenset({NATIVE, REPLAY})),
    # Everything else under the guest CPU runtime.
    ("core/cpu/*", frozenset({CPU, NATIVE})),
    # Tooling no macOS job builds; its python tests run in the changes job and build.yml.
    ("tools/*", NONE),
)


def jobs_for_path(path):
    path = path.replace("\\", "/")
    if path.startswith("./"):
        path = path[2:]
    for pattern, jobs in RULES:
        if fnmatch.fnmatchcase(path, pattern):
            return jobs
    return ALL


def select(paths, labels=()):
    if "upstream-sync" in labels:
        return set(ALL)
    chosen = set()
    for path in paths:
        if path.strip():
            chosen |= jobs_for_path(path.strip())
    return chosen


def changed_files(base, head):
    out = subprocess.run(["git", "diff", "--name-only", f"{base}...{head}"],
                         check=True, capture_output=True, text=True).stdout
    return out.splitlines()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base")
    ap.add_argument("--head")
    ap.add_argument("--labels", default="[]", help="JSON list of PR labels")
    ap.add_argument("--all", action="store_true", help="select every job (push, dispatch)")
    ap.add_argument("--github-output", default=os.environ.get("GITHUB_OUTPUT"))
    args = ap.parse_args(argv)
    if args.all:
        chosen = set(ALL)
    else:
        if not args.base or not args.head:
            ap.error("--base and --head are required unless --all")
        try:
            chosen = select(changed_files(args.base, args.head), json.loads(args.labels))
        except (subprocess.CalledProcessError, ValueError) as err:
            print(f"impact: cannot classify ({err}); running every job", file=sys.stderr)
            chosen = set(ALL)
    lines = [f"{job}={'true' if job in chosen else 'false'}" for job in JOBS]
    print("\n".join(lines))
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
