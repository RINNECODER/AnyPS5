#!/bin/zsh
# Builds rosetta_probe.c for x86_64 and arm64 and runs both. usage: run_probe.sh <scratch-dir>
set -e
HERE=${0:A:h}
OUT=$1
mkdir -p $OUT
clang -arch x86_64 -O1 -o $OUT/probe_x86 $HERE/rosetta_probe.c
clang -arch arm64 -O1 -o $OUT/probe_arm64 $HERE/rosetta_probe.c
echo "== x86_64 (Rosetta)"
arch -x86_64 $OUT/probe_x86
echo "== arm64 (native)"
$OUT/probe_arm64
