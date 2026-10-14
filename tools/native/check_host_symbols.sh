#!/bin/zsh
# Fail if any object defines a symbol the host C library also defines. As a global definition it
# would interpose over libSystem for the whole process and turn every bulk copy or fill in the
# runtime into a no-op (that is what made memset/memcpy/memmove no-ops and produced REPORT16/REPORT18).
#
# usage: check_host_symbols.sh <object.o> [object.o ...]
set -e
sdk=$(xcrun --show-sdk-path)
host=$(mktemp)
trap 'rm -f "$host"' EXIT
nm -g "$sdk"/usr/lib/libSystem.B.tbd "$sdk"/usr/lib/system/*.tbd 2>/dev/null \
    | awk '/^[0-9a-fA-F]+/ { print $3 }' | sort -u > "$host"

bad=$(for object in "$@"; do
    nm -g "$object" 2>/dev/null | awk '($2 == "T" || $2 == "D" || $2 == "B" || $2 == "S") { print $3 }'
done | grep '^_' | sort -u | comm -12 - "$host")

if [[ -n "$bad" ]]; then
    echo "check_host_symbols: objects define host C library symbols:" >&2
    echo "$bad" >&2
    exit 1
fi
