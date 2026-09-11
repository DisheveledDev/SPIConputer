#!/bin/sh
# Refresh the vendored TerminalC sources from the OS repo (canonical:
# ../os/protocol.c, ../os/protocol.h, ../os/font8x8_basic.h). Run this
# after changing the serial-mirror protocol or ROM font on the OS side,
# then rebuild the app.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
os="$here/../../os"
cp "$os/protocol.c" "$here/Sources/TerminalC/protocol.c"
cp "$os/protocol.h" "$here/Sources/TerminalC/include/protocol.h"
cp "$os/font8x8_basic.h" "$here/Sources/TerminalC/include/font8x8_basic.h"
echo "TerminalC synced from $os"
