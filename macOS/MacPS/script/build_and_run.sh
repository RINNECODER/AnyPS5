#!/usr/bin/env bash
set -euo pipefail
MODE="${1:-run}"
case "$MODE" in run|--build-only|--debug|--logs|--verify) ;; *) echo "usage: $0 [run|--build-only|--debug|--logs|--verify]" >&2; exit 2 ;; esac
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
APP_NAME="MacPS"
APP_BUNDLE="$ROOT_DIR/dist/$APP_NAME.app"
# Do not replace a running app or interrupt a game/resource cleanup.
if pgrep -x "$APP_NAME" >/dev/null; then
    echo "Quit MacPS before rebuilding it. Active game sessions must finish cleanup first." >&2
    exit 1
fi
swift build --jobs 2 -Xswiftc -strict-concurrency=complete -Xswiftc -warnings-as-errors
BUILD_BINARY="$(swift build --show-bin-path)/$APP_NAME"
mkdir -p "$APP_BUNDLE/Contents/MacOS" "$APP_BUNDLE/Contents/Resources"
cp "$BUILD_BINARY" "$APP_BUNDLE/Contents/MacOS/$APP_NAME"
cp "$ROOT_DIR/Assets/AppIcon/MacPS.icns" "$APP_BUNDLE/Contents/Resources/MacPS.icns"
BUILD_VERSION="$(date -u +%Y%m%d%H%M%S)"
SOURCE_REVISION="$(git rev-parse --short HEAD)"
cat > "$APP_BUNDLE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>MacPS</string>
<key>CFBundleIdentifier</key><string>com.macps.app</string>
<key>CFBundleName</key><string>MacPS</string>
<key>CFBundleDisplayName</key><string>MacPS</string>
<key>CFBundleIconFile</key><string>MacPS.icns</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.1.0</string>
<key>CFBundleVersion</key><string>$BUILD_VERSION</string>
<key>MacPSSourceRevision</key><string>$SOURCE_REVISION</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>NSPrincipalClass</key><string>NSApplication</string>
</dict></plist>
PLIST
plutil -lint "$APP_BUNDLE/Contents/Info.plist"
codesign --force --sign - "$APP_BUNDLE"
codesign --verify --deep --strict "$APP_BUNDLE"
case "$MODE" in
  --build-only) echo "Built $APP_BUNDLE" ;;
  --debug) lldb -- "$APP_BUNDLE/Contents/MacOS/$APP_NAME" ;;
  --logs) /usr/bin/open "$APP_BUNDLE"; /usr/bin/log stream --info --style compact --predicate 'process == "MacPS"' ;;
  --verify) /usr/bin/open "$APP_BUNDLE"; sleep 1; pgrep -x "$APP_NAME" ;;
  run) /usr/bin/open "$APP_BUNDLE" ;;
esac
