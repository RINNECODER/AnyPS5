#!/usr/bin/env bash
# Runs the x86_64 writer under ANYPS5_GPU_SHARED_MEMORY, then the arm64 verifier on its manifest.
set -u
WRITER=${1:?writer binary}
VERIFIER=${2:?verifier binary}
DIR=${3:?scratch directory}
rm -rf "$DIR"
mkdir -p "$DIR"
MANIFEST="$DIR/manifest.txt"
rm -f "$MANIFEST" "$MANIFEST.done"
export ANYPS5_GPU_SHARED_MEMORY=1
export ANYPS5_GPU_SHARED_MEMORY_MANIFEST="$MANIFEST"
"$WRITER" "$MANIFEST" &
WRITER_PID=$!
for _ in $(seq 1 600); do
    [ -f "$MANIFEST" ] && break
    sleep 0.1
done
if [ ! -f "$MANIFEST" ]; then
    echo "the writer never published a manifest" >&2
    kill "$WRITER_PID" 2>/dev/null
    wait "$WRITER_PID" 2>/dev/null
    exit 1
fi
arch -arm64 "$VERIFIER" "$MANIFEST"
VERIFIER_STATUS=$?
wait "$WRITER_PID"
WRITER_STATUS=$?
if [ "$VERIFIER_STATUS" -ne 0 ] || [ "$WRITER_STATUS" -ne 0 ]; then
    echo "writer $WRITER_STATUS, verifier $VERIFIER_STATUS" >&2
    exit 1
fi
