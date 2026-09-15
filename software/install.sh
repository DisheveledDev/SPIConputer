#!/bin/sh
# Build every card project in this folder and install the products into
# an SD card image (default: software/sdcard, which git ignores):
#
#   software/install.sh [card-image-folder]
#
# The image is laid out like the card - core/ (boot and the shell),
# apps/, utils/, games/, data/ - so it can be copied to a real card, or
# pointed at from the IDE's Settings > SD card image so that running an
# application or utility from the IDE boots this OS.
#
# Each project builds into its own build/ folder first (spibuild), which
# needs the simulator for the .prg step: build it with
#   cmake -S simulator -B simulator/build && cmake --build simulator/build
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
card=${1:-$here/sdcard}
mkdir -p "$card"
card=$(cd "$card" && pwd)

for project in "$here"/*/; do
    [ -f "$project/project.spiproj" ] || continue
    swift run --package-path "$root/ide/macos" spibuild --install "$card" "$project" 2>&1 \
        | grep -v "^Building for\|^\[.*\] \|^Build of product\|Compiling\|Write swift-version\|Planning build"
done
mkdir -p "$card/data"
echo "card image: $card"
ls "$card"
