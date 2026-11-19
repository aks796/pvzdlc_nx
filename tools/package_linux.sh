#!/bin/bash
# Builds the Linux release from a Linux build (tools/build_linux.sh x86_64 / arm64):
#   release/PvZ_DLC_Edition_Linux_<arch>_<version>.tar.gz, one folder "Plants vs. Zombies DLC Edition" holding the
#   program (pvzdlc, debug info stripped), lib/libSDL2-2.0.so.0 (found through the program's own folder), the
#   port's pictures (wide/), README.txt, licenses/, pvzdlc.png (the icon) and cover.png (a Steam library picture).
#   The game unpacks next to them on the first launch (main_posix.c). A tar keeps the program's execute bit.
#   tools/package_linux.sh "Beta 2.5" x86_64
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:?version, e.g. \"Beta 2.5\"}"
ARCH="${2:?x86_64 or arm64}"
case "$ARCH" in x86_64) T=linux ;; arm64) T=linux-arm64 ;; *) echo "x86_64 or arm64"; exit 1 ;; esac
TC="${TOOLCHAIN:-$HOME/sixtyfour/toolchain}"  # (TOOLCHAIN=... for another folder)
STRIP="$TC/llvm-mingw-20261104-ucrt-macos-universal/bin/llvm-strip"  # (LLVM's strip: any architecture's ELF)
B="$ROOT/recomp/build/$T"
NAME="Plants vs. Zombies DLC Edition"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
D="$TMP/$NAME"
mkdir -p "$D/lib"
"$STRIP" --strip-debug -o "$D/pvzdlc" "$B/pvzdlc"
chmod 755 "$D/pvzdlc"
cp -L "$B/sdl/prefix/lib/libSDL2-2.0.so.0" "$D/lib/"
rsync -a --exclude .DS_Store --exclude '*.off' "$ROOT/gamedata/wide/" "$D/wide/"
cp "$ROOT/release/README_linux.txt" "$D/README.txt"
cp -R "$ROOT/release/stage/licenses" "$D/"
cp "$ROOT/icons/pc_icon.png" "$D/pvzdlc.png"
cp "$ROOT/icons/pc_cover.png" "$D/cover.png"
find "$D" -name .DS_Store -delete
if strings -a "$D/pvzdlc" "$D/lib/libSDL2-2.0.so.0" | grep -q "$HOME"; then echo "the program names $HOME"; exit 1; fi
OUT="$ROOT/release/PvZ_DLC_Edition_Linux_${ARCH}_$(echo "$VERSION" | tr -d ' ').tar.gz"
rm -f "$OUT"
# (owned by root in the archive, not by this machine's user; no macOS metadata)
COPYFILE_DISABLE=1 tar --uid 0 --gid 0 --uname root --gname root --no-xattrs --no-mac-metadata -czf "$OUT" -C "$TMP" "$NAME"
if tar -tvzf "$OUT" | grep -q "$(basename "$HOME")"; then echo "the archive names $(basename "$HOME")"; exit 1; fi
echo "built $OUT"
