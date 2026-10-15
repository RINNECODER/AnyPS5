#!/bin/zsh
# Emit a stub object for the symbols a bounded native title image needs but the runtime archives do
# not define. Only `undefined - defined` is stubbed: stubbing every undefined symbol also shadows
# the HLE entry points the runtime implements, which silently turns calls like posix_memalign into
# a bare `ret` and produces misleading crashes.
#
# A stub must never define a symbol the host C library defines. As a global definition it would
# interpose over libSystem for the whole process and turn every bulk copy in the runtime into a
# no-op (that is what made memset/memcpy/memmove no-ops and produced REPORT16/REPORT18). Bare host
# names are never stubbed; guest imports are namespaced `__aps5_guest_<name>` by the driver, and a
# namespaced guest import whose bare name the host defines gets a trampoline to the host function.
#
# usage: stub_undefined.sh <title-object.o> <output-stubs.o> <archive.a> [archive.a ...]
set -e
object=$1
output=$2
shift 2

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

sdk=$(xcrun --show-sdk-path)
# The arm64e blocks are the ones an arm64 macOS binary links against, and only they list the
# linkable host functions: the x86_64-only math builtins (___isfinite, ___isinf, ...) are listed
# in the stub but are not exported on arm64, so a trampoline to one fails the link.
nm -g -arch arm64e "$sdk"/usr/lib/libSystem.B.tbd "$sdk"/usr/lib/system/*.tbd 2>/dev/null \
    | awk '/^[0-9a-fA-F]+/ { print $3 }' | sort -u > "$work/host"

nm -u "$object" | awk '{print $1}' | grep '^_' | sort -u > "$work/undefined"
for archive in "$@"; do
    nm -g "$archive" 2>/dev/null
done | awk '($2 == "T" || $2 == "D" || $2 == "B" || $2 == "S" || $2 == "W") { print $3 }' \
    | grep '^_' | sort -u > "$work/defined"

# Never stub a host C library symbol: the reference must resolve to libSystem.
comm -23 "$work/undefined" "$work/defined" | comm -23 - "$work/host" > "$work/names"
if comm -12 "$work/undefined" "$work/host" | comm -12 - "$work/defined" | grep -q .; then
    echo "stub_undefined: an archive defines a host C library symbol" >&2
fi

python3 - "$work/names" "$work/host" "$work/stubs.s" <<'PY'
import sys
names = [line.strip() for line in open(sys.argv[1]) if line.strip()]
host = set(line.strip() for line in open(sys.argv[2]) if line.strip())
prefix = '__aps5_guest_'
lines = ['.text']
for name in names:
    # private_extern: the image's own code resolves it, but the executable does not export it.
    lines.append(f'.private_extern {name}')
    lines.append(f'{name}:')
    # nm reports Mach-O symbols with one leading underscore; strip it before the guest prefix so a
    # namespaced guest import of a host C library function gets the trampoline instead of a bare
    # `ret` (which returned a garbage length from strlen and faulted the heap initialiser).
    symbol = name[1:] if name.startswith('_') else name
    bare = '_' + symbol[len(prefix):] if symbol.startswith(prefix) else None
    if bare is not None and bare in host:
        lines.append(f'  b {bare}')
    else:
        lines.append('  ret')
open(sys.argv[3], 'w').write('\n'.join(lines) + '\n')
print(f'stubbed {len(names)} symbols')
PY

xcrun clang -arch arm64 -c "$work/stubs.s" -o "$output"
