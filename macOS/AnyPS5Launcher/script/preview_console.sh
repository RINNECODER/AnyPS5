#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
MODE="${1:-run}"
case "$MODE" in run|--build-only) ;; *) echo "usage: $0 [run|--build-only]" >&2; exit 2 ;; esac
SCRATCH="$ROOT_DIR/.build-ui-preview"
APP="$ROOT_DIR/dist/MacPSPreview.app"
STATE="$ROOT_DIR/.preview-state"
swift build --scratch-path "$SCRATCH" --jobs 2 -Xswiftc -strict-concurrency=complete -Xswiftc -warnings-as-errors
BIN="$(swift build --scratch-path "$SCRATCH" --show-bin-path)"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$STATE"
cp "$BIN/AnyPS5Launcher" "$APP/Contents/MacOS/MacPSPreview"
# SwiftPM's generated accessor resolves packaged resources under Contents/Resources.
cp -R "$BIN/AnyPS5Launcher_AnyPS5Launcher.bundle" "$APP/Contents/Resources/"
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>MacPSPreview</string>
<key>CFBundleIdentifier</key><string>com.macps.console.preview</string>
<key>CFBundleName</key><string>MacPS Preview</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>NSPrincipalClass</key><string>NSApplication</string>
</dict></plist>
PLIST
plutil -insert MacPSPreviewStateDirectory -string "$STATE" "$APP/Contents/Info.plist"
# Read only the saved catalogue; never copy live executables, modules, or engine selection.
python3 - "$STATE" <<'PY'
from pathlib import Path
import json,sys
state=Path(sys.argv[1])
cache=Path.home()/'Library/Application Support/AnyPS5Launcher/orbit-catalogue-v2.json'
library={'games':[], 'enginePath':''}
if cache.is_file():
    raw=cache.read_bytes()
    catalogue=json.loads(raw)
    (state/'orbit-catalogue-v2.json').write_bytes(raw)
    releases=catalogue.get('releases',[])
    chosen=[]
    for keyword in ['civilization','gran turismo','elden ring','horizon','spider-man','god of war']:
        release=next((r for r in releases if keyword in r.get('title','').lower() and r.get('gameId') not in chosen),None)
        if release:
            chosen.append(release['gameId'])
            library['games'].append({'id':release['gameId'],'title':release['title'],
                'executablePath':'/nonexistent/macps-ui-preview/'+release['gameId']+'.elf',
                'workingDirectory':str(state),'sceModulePaths':[]})
(state/'library.json').write_text(json.dumps(library,indent=2)+'\n')
PY
codesign --force --sign - "$APP"
codesign --verify --deep --strict "$APP"
if [[ "$MODE" == "--build-only" ]]; then
    echo "Built isolated preview: $APP"
else
    /usr/bin/open -n "$APP" --args --macps-preview --preview-state "$STATE"
fi
