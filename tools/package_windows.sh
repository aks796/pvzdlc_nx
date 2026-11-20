#!/bin/bash
# Builds the Windows release from the win64 build (recomp: make TARGET=win64):
#   release/PvZ_DLC_Edition_Windows_<version>.zip, one folder "Plants vs. Zombies DLC Edition" holding the app
#   ("PvZ DLC Edition.exe", debug info stripped), SDL2.dll (2.32.10's mingw build), the port's pictures (wide/),
#   README.txt and licenses/. The game unpacks next to them on the first launch (main_posix.c).
#   tools/package_windows.sh "Beta 2.5.1"
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:?version, e.g. \"Beta 2.5.1\"}"
TC="${TOOLCHAIN:-$HOME/sixtyfour/toolchain}"  # (TOOLCHAIN=... for another folder)
STRIP="$TC/llvm-mingw-20261104-ucrt-macos-universal/bin/x86_64-w64-mingw32-strip"
NAME="Plants vs. Zombies DLC Edition"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
D="$TMP/$NAME"
mkdir -p "$D"
"$STRIP" --strip-debug -o "$D/PvZ DLC Edition.exe" "$ROOT/recomp/build/win64/pvzdlc.exe"
cp "$TC/SDL2-2.32.10/x86_64-w64-mingw32/bin/SDL2.dll" "$D/"
rsync -a --exclude .DS_Store --exclude '*.off' "$ROOT/gamedata/wide/" "$D/wide/"
cp "$ROOT/release/README_windows.txt" "$D/README.txt"
cp -R "$ROOT/release/stage/licenses" "$D/"
cp "$ROOT/icons/pc_cover.png" "$D/cover.png"  # (a Steam library picture: README_windows.txt)
find "$D" -name .DS_Store -delete
if strings -a "$D/PvZ DLC Edition.exe" "$D/SDL2.dll" | grep -q "$HOME"; then echo "the app names $HOME"; exit 1; fi
ZIP="$ROOT/release/PvZ_DLC_Edition_Windows_$(echo "$VERSION" | tr -d ' ').zip"
rm -f "$ZIP"
(cd "$TMP" && zip -q -r -X "$ZIP" "$NAME")
echo "built $ZIP"
