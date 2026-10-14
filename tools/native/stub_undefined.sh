#!/bin/zsh
# Emit a stub object for the symbols a bounded native title image needs but the runtime archives do
# not define. Only `undefined - defined` is stubbed: stubbing every undefined symbol also shadows
# the HLE entry points the runtime implements, which silently turns calls like posix_memalign into
# a bare `ret` and produces misleading crashes.
#
# usage: stub_undefined.sh <title-object.o> <output-stubs.o> <archive.a> [archive.a ...]
set -e
object=$1
output=$2
shift 2

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

nm -u "$object" | awk '{print $1}' | grep '^_' | sort -u > "$work/undefined"
for archive in "$@"; do
    nm -g "$archive" 2>/dev/null
done | awk '($2 == "T" || $2 == "D" || $2 == "B" || $2 == "S" || $2 == "W") { print $3 }' \
    | grep '^_' | sort -u > "$work/defined"
comm -23 "$work/undefined" "$work/defined" > "$work/names"

python3 - "$work/names" "$work/stubs.s" <<'PY'
import sys
names = [line.strip() for line in open(sys.argv[1]) if line.strip()]
lines = ['.text']
for name in names:
    lines += [f'.globl {name}', f'{name}:', '  ret']
open(sys.argv[2], 'w').write('\n'.join(lines) + '\n')
print(f'stubbed {len(names)} symbols')
PY

xcrun clang -arch arm64 -c "$work/stubs.s" -o "$output"
