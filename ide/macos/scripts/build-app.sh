#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
IDE_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
ROOT_DIR=$(CDPATH= cd -- "$IDE_DIR/../.." && pwd)
BUILD_DIR="$ROOT_DIR/simulator/build-app"
DIST_DIR="$IDE_DIR/dist"
APP="$DIST_DIR/SPIComputer IDE.app"
APP_CONTENTS="$APP/Contents"
APP_MACOS="$APP_CONTENTS/MacOS"
APP_RESOURCES="$APP_CONTENTS/Resources"
APP_FRAMEWORKS="$APP_CONTENTS/Frameworks"

if [ -n "${CMAKE:-}" ]; then
    CMAKE_BIN="$CMAKE"
elif command -v cmake >/dev/null 2>&1; then
    CMAKE_BIN=$(command -v cmake)
elif [ -x "$HOME/.pico-sdk/cmake/v4.3.4/bin/cmake" ]; then
    CMAKE_BIN="$HOME/.pico-sdk/cmake/v4.3.4/bin/cmake"
else
    printf '%s\n' 'cmake is required to build the simulator' >&2
    exit 1
fi

"$CMAKE_BIN" -S "$ROOT_DIR/simulator" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
"$CMAKE_BIN" --build "$BUILD_DIR"
cd "$IDE_DIR"
swift build -c release --product SPIIDE
SWIFT_BIN_DIR=$(swift build -c release --show-bin-path)
SIMULATOR="$BUILD_DIR/spicomputer_sim"
IDE_EXECUTABLE="$SWIFT_BIN_DIR/SPIIDE"
RESOURCE_BUNDLE="$SWIFT_BIN_DIR/SPIIDE_SPIIDE.bundle"

[ -x "$SIMULATOR" ] || { printf '%s\n' "missing simulator: $SIMULATOR" >&2; exit 1; }
[ -x "$IDE_EXECUTABLE" ] || { printf '%s\n' "missing IDE executable: $IDE_EXECUTABLE" >&2; exit 1; }
[ -d "$RESOURCE_BUNDLE" ] || { printf '%s\n' "missing resource bundle: $RESOURCE_BUNDLE" >&2; exit 1; }

rm -rf "$APP"
mkdir -p "$APP_MACOS" "$APP_RESOURCES/simulator" "$APP_FRAMEWORKS"
cp "$IDE_EXECUTABLE" "$APP_MACOS/SPIIDE"
ditto "$RESOURCE_BUNDLE" "$APP/SPIIDE_SPIIDE.bundle"
cp "$RESOURCE_BUNDLE/AppIcon.png" "$APP_RESOURCES/AppIcon.png"
cp "$SIMULATOR" "$APP_RESOURCES/simulator/spicomputer_sim"
chmod 755 "$APP_MACOS/SPIIDE" "$APP_RESOURCES/simulator/spicomputer_sim"

SDL_PATH=$(otool -L "$SIMULATOR" | awk '/libSDL2/ {print $1; exit}')
if [ -n "$SDL_PATH" ] && [ -f "$SDL_PATH" ]; then
    SDL_NAME=$(basename "$SDL_PATH")
    cp "$SDL_PATH" "$APP_FRAMEWORKS/$SDL_NAME"
    install_name_tool -change "$SDL_PATH" "@rpath/$SDL_NAME" "$APP_RESOURCES/simulator/spicomputer_sim"
    install_name_tool -add_rpath '@loader_path/../../Frameworks' "$APP_RESOURCES/simulator/spicomputer_sim"
    install_name_tool -id "@rpath/$SDL_NAME" "$APP_FRAMEWORKS/$SDL_NAME"
fi

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

printf '%s\n' "$APP"
