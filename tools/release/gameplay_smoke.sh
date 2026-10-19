#!/usr/bin/env bash
# Pre-release gameplay gate: boot the title the way MacPS does, drive it to the 3D map, and require
# it to keep playing there. usage: gameplay_smoke.sh --build <rosetta build dir> [options]
#
#   --build DIR        x86_64 build dir configured with -DANYPS5_ROSETTA_LIBS=ON (required)
#   --title-dir DIR    title dump: eboot.bin and the .prx modules MacPS passes (or ANYPS5_E2E_TITLE_DIR)
#   --from-save DIR    a title-data tree holding one autosave, loaded through Load Game > Autosaves
#   --out DIR          output directory (default: ./gameplay-smoke-<timestamp>)
#   --seconds N        wall limit for the run (default 320)
#   --min-gameplay N   seconds the guest must keep presenting frames after the map appears (default 60)
#
# The gate passes only when the guest exits cleanly (or reaches the wall limit), no fault or uncaught
# exception is reported, a detailed 3D frame appears after map generation, frames keep presenting for
# --min-gameplay seconds after it, and that frame is saved as map.png. Refuses to run while Steam or
# the title is running, and takes the game-run lock (with-lock eboot) when that helper is installed.
set -u
BUILD=""; TITLE=""; FROMSAVE=""; OUT=""; SECONDS_LIMIT=320; MIN_GAMEPLAY=60
while [ $# -gt 0 ]; do case $1 in
  --build) BUILD=$2; shift;;
  --title-dir) TITLE=$2; shift;;
  --from-save) FROMSAVE=$2; shift;;
  --out) OUT=$2; shift;;
  --seconds) SECONDS_LIMIT=$2; shift;;
  --min-gameplay) MIN_GAMEPLAY=$2; shift;;
  *) echo "unknown argument: $1" >&2; exit 2;;
esac; shift; done
[ -n "$BUILD" ] || { sed -n '2,14p' "$0"; exit 2; }
BUILD=$(cd "$BUILD" && pwd)
TITLE=${TITLE:-${ANYPS5_E2E_TITLE_DIR:-}}
[ -n "$TITLE" ] || { echo "--title-dir or ANYPS5_E2E_TITLE_DIR is required" >&2; exit 2; }
TITLE=$(cd "$TITLE" && pwd)
SRC=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt")
[ -x "$BUILD/rosetta/bin/anyps5_rosetta_run" ] && [ -f "$SRC/tools/rosetta/run_title.py" ] || { echo "bad build dir: $BUILD" >&2; exit 2; }
OUT=${OUT:-$PWD/gameplay-smoke-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT/frames"

if pgrep -f steam_osx >/dev/null; then echo "Steam is running; the gate needs the GPU to itself" >&2; exit 2; fi
if pgrep -f anyps5_rosetta_run >/dev/null; then echo "a title is already running" >&2; exit 2; fi

SCRIPT=""
if [ -n "$FROMSAVE" ]; then
  [ -d "$FROMSAVE" ] || { echo "bad --from-save: $FROMSAVE" >&2; exit 2; }
  CYC=(down cross r1 down cross); i=0
  for ((t = 8; t <= 150; t += 3)); do SCRIPT="$SCRIPT${SCRIPT:+;}t=$t:${CYC[i % 5]}"; i=$((i + 1)); done
  for ((t = 154; t <= SECONDS_LIMIT; t += 6)); do SCRIPT="$SCRIPT;t=$t:cross"; done
else
  B=cross
  for ((t = 8; t <= SECONDS_LIMIT; t += 3)); do SCRIPT="$SCRIPT${SCRIPT:+;}t=$t:$B"; [ $B = cross ] && B=square || B=cross; done
fi

S=$(mktemp -d /private/tmp/anyps5-gameplay-smoke.XXXXXX)
cleanup() { rm -rf "$S"; }
trap cleanup EXIT
TITLE_DATA="$S/titledata"; mkdir -p "$TITLE_DATA"
[ -n "$FROMSAVE" ] && cp -R "$FROMSAVE/." "$TITLE_DATA/"

export ANYPS5_INPUT_SCRIPT="$SCRIPT"
export ANYPS5_TITLE_DATA="$TITLE_DATA"
export ANYPS5_FRAME_CAPTURE_DIR="$OUT/frames"
export ANYPS5_FRAME_CAPTURE_INTERVAL_MS=3000
export ANYPS5_UNOBTRUSIVE_WINDOWS=1

RUN=(python3 "$SRC/tools/rosetta/run_title.py" "$TITLE" --build "$BUILD" --wall "$SECONDS_LIMIT" --log "$OUT/run.log")
if command -v with-lock >/dev/null 2>&1; then with-lock eboot "${RUN[@]}" > "$OUT/summary.txt" 2>&1; else "${RUN[@]}" > "$OUT/summary.txt" 2>&1; fi

python3 - "$OUT" "$SECONDS_LIMIT" "$MIN_GAMEPLAY" <<'PY'
import json, os, re, shutil, sys
out, limit, minimum = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
lines = open(os.path.join(out, 'run.log'), errors='replace').read().splitlines()
t0 = None
for l in lines:
    m = re.search(r'input script t=([\d.]+):\w+: down at [\d.]+ s \(epoch ms (\d+)\)', l)
    if m:
        t0 = int(m.group(2)) - float(m.group(1)) * 1000
        break
map_index = None; map_time = None; map_frame = None
generated = False
for i, l in enumerate(lines):
    if 'scripts/voronoi' in l:
        generated = True
    if map_index is None and 'frame_captured' in l:
        try: event = json.loads(l)
        except ValueError: continue
        if event.get('nonblack_ratio', 0) >= 0.5 and event.get('distinct_colors', 0) >= 512:
            m = re.search(r'frame-\d+-(\d+)\.png', event.get('path', ''))
            if m and t0:
                map_index = i
                map_time = (int(m.group(1)) - t0) / 1000.0
                map_frame = event['path']
exits = [json.loads(l) for l in lines if l.startswith('{') and '"event":"guest_exit"' in l]
exit_code = exits[-1].get('exit_code') if exits else None
faulted = any(re.search(r'guest fault|uncaught_exception|assertion failed', l) for l in lines)
play = (limit - map_time) if map_time is not None else 0.0
after = sum(1 for l in lines[map_index:] if '"event":"frames_presented"' in l) if map_index is not None else 0
screenshot = os.path.join(out, 'map.png')
if map_frame and os.path.exists(map_frame):
    shutil.copyfile(map_frame, screenshot)
checks = [
    ('map generation reached', generated),
    ('3D map frame captured', map_time is not None),
    ('frames keep presenting after the map', after >= max(1, int(minimum / 3))),
    (f'gameplay >= {minimum:.0f} s after the map', play >= minimum),
    ('clean guest exit', exit_code == 0 and not faulted),
    ('map screenshot saved', os.path.exists(screenshot)),
]
for name, ok in checks:
    print(('PASS ' if ok else 'FAIL ') + name)
print(f'map at {map_time if map_time is None else round(map_time, 1)} s, wall {limit:.0f} s, gameplay {play:.1f} s, '
      f'frames_presented events after map {after}, exit {exit_code}')
print('out: ' + out)
sys.exit(0 if all(ok for _, ok in checks) else 1)
PY
