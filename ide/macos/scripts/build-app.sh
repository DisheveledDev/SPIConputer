#!/bin/sh
# Produces dist/SPIComputer IDE.app from this package alone: the IDE
# executable, its resource bundles (the app icon; the SPIIDECore bundle
# with the SDK frameworks, the vendored simulator + SDL library and the
# minimal card image) and an Info.plist. Nothing outside this repository
# is needed; refresh the vendored simulator and OS with
# scripts/update-vendor.sh when the OS workspace changes.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
IDE_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DIST_DIR="$IDE_DIR/dist"
APP="$DIST_DIR/SPIComputer IDE.app"
APP_CONTENTS="$APP/Contents"
APP_MACOS="$APP_CONTENTS/MacOS"
APP_RESOURCES="$APP_CONTENTS/Resources"

cd "$IDE_DIR"
swift build -c release --product SPIIDE
SWIFT_BIN_DIR=$(swift build -c release --show-bin-path)
IDE_EXECUTABLE="$SWIFT_BIN_DIR/SPIIDE"
APP_BUNDLE="$SWIFT_BIN_DIR/SPIIDE_SPIIDE.bundle"
CORE_BUNDLE="$SWIFT_BIN_DIR/SPIIDE_SPIIDECore.bundle"

[ -x "$IDE_EXECUTABLE" ] || { printf '%s\n' "missing IDE executable: $IDE_EXECUTABLE" >&2; exit 1; }
[ -d "$APP_BUNDLE" ] || { printf '%s\n' "missing resource bundle: $APP_BUNDLE" >&2; exit 1; }
[ -d "$CORE_BUNDLE" ] || { printf '%s\n' "missing resource bundle: $CORE_BUNDLE" >&2; exit 1; }
[ -x "$CORE_BUNDLE/simulator/spicomputer_sim" ] || {
    printf '%s\n' "the vendored simulator is missing or not executable: run scripts/update-vendor.sh" >&2
    exit 1
}

rm -rf "$APP"
mkdir -p "$APP_MACOS" "$APP_RESOURCES"
cp "$IDE_EXECUTABLE" "$APP_MACOS/SPIIDE"
chmod 755 "$APP_MACOS/SPIIDE"
# SwiftPM resource bundles are looked up next to the executable's bundle
# (Bundle.module), so they sit at the app's top level.
ditto "$APP_BUNDLE" "$APP/SPIIDE_SPIIDE.bundle"
ditto "$CORE_BUNDLE" "$APP/SPIIDE_SPIIDECore.bundle"
chmod 755 "$APP/SPIIDE_SPIIDECore.bundle/simulator/spicomputer_sim"
cp "$APP_BUNDLE/AppIcon.png" "$APP_RESOURCES/AppIcon.png"

cat > "$APP_CONTENTS/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDisplayName</key>
    <string>SPIComputer IDE</string>
    <key>CFBundleExecutable</key>
    <string>SPIIDE</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon.png</string>
    <key>CFBundleIdentifier</key>
    <string>com.spicomputer.ide</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>SPIComputer IDE</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
</dict>
</plist>
EOF

# Ad-hoc signature so the app launches on Apple silicon after copying.
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || true

printf '%s\n' "$APP"
