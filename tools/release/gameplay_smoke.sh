#!/usr/bin/env bash
# Pre-release gameplay gate: boot the title the way MacPS does, drive it to the 3D map, and require
# it to keep playing there. usage: gameplay_smoke.sh --build <rosetta build dir> [options]
#
#   --build DIR        x86_64 build dir configured with -DANYPS5_ROSETTA_LIBS=ON (required)
#   --title-dir DIR    title dump: eboot.bin and the .prx modules MacPS passes (or ANYPS5_E2E_TITLE_DIR)
#   --from-save DIR    a title-data tree holding one autosave, loaded through Load Game > Autosaves
#                      (best effort; the default drives a new game to the map)
#   --out DIR          output directory (default: ./gameplay-smoke-<timestamp>)
#   --seconds N        wall limit for the run (default 320)
#   --min-gameplay N   seconds the guest must keep presenting frames after the map appears (default 60)
#   --max-black N      frames after the map allowed below --black-ratio (default 4)
#   --black-ratio F    a frame below this nonblack_ratio is a black-region frame (default 0.65)
#
# The controller watches the engine's log and its captured frames and presses only when it sees the
# screen that needs it (the main menu, then the leader select), so it does not depend on when a
# screen appears. It stops pressing once map generation starts, waits for a detailed 3D frame, then
# keeps playing for --min-gameplay seconds. The gate passes only when the guest exits cleanly (or
# reaches the wall limit) with no fault, the 3D map appears, frames keep presenting after it, no
# black-region frames appear, and the map screenshot is saved as map.png. Refuses to run while Steam
# or the title is running, and takes the game-run lock (with-lock eboot) when that helper is installed.
set -u
BUILD=""; TITLE=""; FROMSAVE=""; OUT=""; SECONDS_LIMIT=320; MIN_GAMEPLAY=60; MAX_BLACK=4; BLACK_RATIO=0.65
while [ $# -gt 0 ]; do case $1 in
  --build) BUILD=$2; shift;;
  --title-dir) TITLE=$2; shift;;
  --from-save) FROMSAVE=$2; shift;;
  --out) OUT=$2; shift;;
  --seconds) SECONDS_LIMIT=$2; shift;;
  --min-gameplay) MIN_GAMEPLAY=$2; shift;;
  --max-black) MAX_BLACK=$2; shift;;
  --black-ratio) BLACK_RATIO=$2; shift;;
  *) echo "unknown argument: $1" >&2; exit 2;;
esac; shift; done
[ -n "$BUILD" ] || { sed -n '2,19p' "$0"; exit 2; }
BUILD=$(cd "$BUILD" && pwd)
TITLE=${TITLE:-${ANYPS5_E2E_TITLE_DIR:-}}
[ -n "$TITLE" ] || { echo "--title-dir or ANYPS5_E2E_TITLE_DIR is required" >&2; exit 2; }
TITLE=$(cd "$TITLE" && pwd)
SRC=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt")
[ -x "$BUILD/rosetta/bin/anyps5_rosetta_run" ] && [ -f "$SRC/tools/rosetta/run_title.py" ] || { echo "bad build dir: $BUILD" >&2; exit 2; }
OUT=${OUT:-$PWD/gameplay-smoke-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"

if pgrep -f steam_osx >/dev/null; then echo "Steam is running; the gate needs the GPU to itself" >&2; exit 2; fi
if pgrep -f anyps5_rosetta_run >/dev/null; then echo "a title is already running" >&2; exit 2; fi

CONTROLLER=$(mktemp /private/tmp/anyps5-gameplay-controller.XXXXXX.py)
trap 'rm -f "$CONTROLLER"' EXIT
cat > "$CONTROLLER" <<'PY'
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BUTTONS = {"cross": 0x4000, "square": 0x8000, "circle": 0x2000, "down": 0x40, "up": 0x10, "r1": 0x800}

build, title, source, out = (Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
seconds, minimum, max_black, black_ratio = (float(sys.argv[5]), float(sys.argv[6]), int(sys.argv[7]), float(sys.argv[8]))
from_save = sys.argv[9]

frames = out / 'frames'
frames.mkdir(parents=True, exist_ok=True)
scratch = Path(tempfile.mkdtemp(prefix='anyps5-gameplay-smoke-'))
pad = scratch / 'pad'
title_data = scratch / 'titledata'
title_data.mkdir()
if from_save:
    shutil.copytree(from_save, title_data, dirs_exist_ok=True)
run_log = out / 'run.log'

env = dict(os.environ)
env['APS5_PAD_INPUT_FILE'] = str(pad)
env['ANYPS5_TITLE_DATA'] = str(title_data)
env['ANYPS5_FRAME_CAPTURE_DIR'] = str(frames)
env['ANYPS5_FRAME_CAPTURE_INTERVAL_MS'] = '1000'
env['ANYPS5_UNOBTRUSIVE_WINDOWS'] = '1'
env.pop('ANYPS5_INPUT_SCRIPT', None)

command = ['python3', str(source / 'tools' / 'rosetta' / 'run_title.py'), str(title),
           '--build', str(build), '--wall', str(int(seconds)), '--log', str(run_log)]
summary = open(out / 'summary.txt', 'w')
engine = subprocess.Popen(command, env=env, stdout=summary, stderr=subprocess.STDOUT, start_new_session=True)

sequence = 0
def press(button, hold=250):
    global sequence
    sequence += 1
    pad.write_text(f'{sequence} {BUTTONS[button]} {hold}\n')

start = time.monotonic()
log_position = 0
menu_seen = leader_seen = map_generation_seen = False
menu_pressed = False
last_press = -10.0
press_count = 0
map_time = None
map_frame = None
after = []
presented_after = 0
save_queue = []
while True:
    now = time.monotonic() - start
    try:
        with open(run_log, 'r', errors='replace') as stream:
            stream.seek(log_position)
            fresh = stream.read()
            log_position = stream.tell()
    except FileNotFoundError:
        fresh = ''
    for line in fresh.splitlines():
        if 'TEXTURE_my2k_notloggedin' in line:
            menu_seen = True
        if 'TEXTURE_age_sel_bg_ramp' in line:
            leader_seen = True
        if 'scripts/voronoi' in line:
            map_generation_seen = True
        if '"event":"frames_presented"' in line and map_time is not None:
            presented_after += 1
        if 'frame_captured' in line:
            try:
                event = json.loads(line)
            except ValueError:
                continue
            ratio = event.get('nonblack_ratio', 1.0)
            colors = event.get('distinct_colors', 0)
            if map_time is None and map_generation_seen and ratio >= 0.6:
                map_time = now
                map_frame = event.get('path')
            if map_time is not None:
                after.append((now, ratio, colors, event.get('path')))
    if map_time is None and not map_generation_seen:
        if from_save:
            if menu_seen and not menu_pressed:
                menu_pressed = True
                save_queue = [(now, 'down'), (now + 0.8, 'cross'), (now + 1.7, 'r1'), (now + 2.5, 'down'), (now + 3.2, 'cross')]
            while save_queue and save_queue[0][0] <= now:
                press(save_queue.pop(0)[1])
                last_press = now
            if not save_queue and now - last_press >= 2.5:
                press('cross')
                last_press = now
        else:
            if menu_seen and not menu_pressed:
                menu_pressed = True
                press('cross')
                last_press = now
            elif now - last_press >= 2.5:
                press('square' if press_count % 2 else 'cross')
                press_count += 1
                last_press = now
    if engine.poll() is not None:
        break
    if map_time is not None and now - map_time >= minimum + 5:
        break
    if now >= seconds + 30:
        break
    time.sleep(0.25)

if engine.poll() is None:
    try:
        os.killpg(engine.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        engine.wait(timeout=20)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(engine.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        engine.wait()
summary.close()

lines = run_log.read_text(errors='replace').splitlines() if run_log.exists() else []
exits = [json.loads(l) for l in lines if l.startswith('{') and '"event":"guest_exit"' in l]
exit_code = exits[-1].get('exit_code') if exits else None
faulted = any(re.search(r'guest fault|uncaught_exception|assertion failed', l) for l in lines)
black = [entry for entry in after if entry[1] < black_ratio]
run = worst = 0
for entry in after:
    run = run + 1 if entry[1] < black_ratio else 0
    worst = max(worst, run)
play = (after[-1][0] - map_time) if (map_time is not None and after) else 0.0
screenshot = out / 'map.png'
if map_frame and Path(map_frame).exists():
    shutil.copyfile(map_frame, screenshot)
    best = max(after, key=lambda entry: entry[1]) if after else None
    if best and Path(best[3]).exists():
        shutil.copyfile(best[3], out / 'map-best.png')
checks = [
    ('3D map frame captured', map_time is not None),
    (f'gameplay >= {minimum:.0f} s after the map', play >= minimum),
    ('frames keep presenting after the map', presented_after >= max(1, int(minimum / 3))),
    (f'no black-region frames (< {black_ratio})', len(black) <= max_black),
    ('clean guest exit', exit_code == 0 and not faulted),
    ('map screenshot saved', screenshot.exists()),
]
for name, ok in checks:
    print(('PASS ' if ok else 'FAIL ') + name)
ratios = [entry[1] for entry in after]
print(f'map at {round(map_time, 1) if map_time else None} s, gameplay {play:.1f} s, captured after map {len(after)}, '
      f'presented events {presented_after}, nonblack min {round(min(ratios), 3) if ratios else None} '
      f'mean {round(sum(ratios) / len(ratios), 3) if ratios else None}, black frames {len(black)}, '
      f'longest black run {worst}, exit {exit_code}')
print('out: ' + str(out))
shutil.rmtree(scratch, ignore_errors=True)
sys.exit(0 if all(ok for _, ok in checks) else 1)
PY

if command -v with-lock >/dev/null 2>&1; then
  with-lock eboot python3 "$CONTROLLER" "$BUILD" "$TITLE" "$SRC" "$OUT" "$SECONDS_LIMIT" "$MIN_GAMEPLAY" "$MAX_BLACK" "$BLACK_RATIO" "$FROMSAVE"
else
  python3 "$CONTROLLER" "$BUILD" "$TITLE" "$SRC" "$OUT" "$SECONDS_LIMIT" "$MIN_GAMEPLAY" "$MAX_BLACK" "$BLACK_RATIO" "$FROMSAVE"
fi
