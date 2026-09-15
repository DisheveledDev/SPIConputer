#!/bin/sh
# Refreshes what this package vendors from the SPIComputer OS workspace:
#
#   scripts/update-vendor.sh [workspace-root]
#
# - Sources/SPIIDECore/Resources/simulator/: a Release build of the OS
#   simulator with its SDL2 library beside it, rewritten to load the
#   library from its own folder (@rpath = @loader_path) and ad-hoc
#   signed, so the same files work from the package and inside the app;
# - Sources/SPIIDECore/Resources/sdcard/: a minimal working card: core/
#   (boot, the shell and the APPS launcher) and utils/ (every utility
#   project: the shell's DIR, COPY, HELP ... are utilities), built with
#   spibuild from the workspace's software/ folder.
#
# The workspace root defaults to the folder that contains this
# repository (the layout while the IDE lived in the OS repository); pass
# the path to the OS checkout otherwise. Needs cmake and SDL2 (Homebrew).
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
IDE_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
WORKSPACE=${1:-$(CDPATH= cd -- "$IDE_DIR/../.." && pwd)}
VENDOR="$IDE_DIR/Sources/SPIIDECore/Resources"
BUILD_DIR="$WORKSPACE/simulator/build-vendor"

[ -f "$WORKSPACE/simulator/CMakeLists.txt" ] || {
    printf '%s\n' "no OS workspace at $WORKSPACE (expected simulator/CMakeLists.txt)" >&2
    exit 1
}

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

"$CMAKE_BIN" -S "$WORKSPACE/simulator" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
"$CMAKE_BIN" --build "$BUILD_DIR"

rm -rf "$VENDOR/sdcard"
mkdir -p "$VENDOR/simulator" "$VENDOR/sdcard/core" "$VENDOR/sdcard/utils"
SIM="$VENDOR/simulator/spicomputer_sim"
cp "$BUILD_DIR/spicomputer_sim" "$SIM"
chmod 755 "$SIM"
SDL_PATH=$(otool -L "$SIM" | awk '/libSDL2/ {print $1; exit}')
if [ -n "$SDL_PATH" ] && [ -f "$SDL_PATH" ]; then
    SDL_NAME=$(basename "$SDL_PATH")
    cp "$SDL_PATH" "$VENDOR/simulator/$SDL_NAME"
    chmod 644 "$VENDOR/simulator/$SDL_NAME"
    install_name_tool -change "$SDL_PATH" "@rpath/$SDL_NAME" "$SIM"
    install_name_tool -add_rpath '@loader_path' "$SIM" 2>/dev/null || true
    install_name_tool -id "@rpath/$SDL_NAME" "$VENDOR/simulator/$SDL_NAME"
    codesign --force --sign - "$SIM" "$VENDOR/simulator/$SDL_NAME"
fi

# The OS: boot, the shell and the launcher, plus every utility project,
# built and installed by this package's own spibuild (which uses the
# simulator just vendored for the .prg step).
cd "$IDE_DIR"
for project in boot os apps; do
    swift run spibuild --install "$VENDOR/sdcard" "$WORKSPACE/software/$project"
done
for manifest in "$WORKSPACE"/software/*/project.spiproj; do
    if grep -Eq '"kind" *: *"utility"' "$manifest"; then
        swift run spibuild --install "$VENDOR/sdcard" "$(dirname "$manifest")"
    fi
done

printf 'vendored simulator: %s\n' "$("$SIM" --help 2>&1 | head -1)"
ls -la "$VENDOR/simulator" "$VENDOR/sdcard/core"
ls "$VENDOR/sdcard/utils"
