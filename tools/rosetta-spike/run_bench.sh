#!/bin/zsh
# Rosetta vs TCG micro-benchmark. usage: run_bench.sh <scratch-dir> <python-with-unicorn> [iters]
set -e
HERE=${0:A:h}
OUT=$1; PY=$2; N=${3:-200000000}
mkdir -p $OUT
CF=(-O2 -fno-jump-tables -fno-stack-protector -fno-builtin)
clang -arch x86_64 $CF -o $OUT/host_x86 $HERE/host.c $HERE/kernel.c
clang -arch x86_64 $CF -mavx2 -mfma -mbmi2 -o $OUT/host_x86_avx2 $HERE/host.c $HERE/kernel.c
clang -arch arm64 $CF -o $OUT/host_arm64 $HERE/host.c $HERE/kernel.c
arch -x86_64 $OUT/host_x86 --dump $OUT/kernel.bin
echo "native arm64:      $($OUT/host_arm64 $N)"
echo "rosetta x86_64:    $(arch -x86_64 $OUT/host_x86 $N)"
echo "rosetta avx2/fma:  $(arch -x86_64 $OUT/host_x86_avx2 $N)"
echo "insn count:        $($PY -I $HERE/bench_tcg.py $OUT/kernel.bin 100000 --count-insns)"
echo "rosetta x86_64 1e5: $(arch -x86_64 $OUT/host_x86 100000)"
echo "tcg (unicorn):     $($PY -I $HERE/bench_tcg.py $OUT/kernel.bin $((N / 10)))"
