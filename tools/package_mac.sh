#!/bin/bash
# Builds the Mac app from the play build (recomp/build/play/pvzdlc_host, `make TARGET=play`):
#   release/mac/Plants vs. Zombies DLC Edition.app  and  release/PvZ_DLC_Edition_Mac_<version>.zip
# The app holds the game code, SDL2 (Homebrew's, its install name pointed into the app), the port's pictures
# (gamedata/wide) and its icon (icons/pc_icon.png). It is signed ad hoc (Apple Silicon runs nothing unsigned); it has no
# developer ID, so the README tells players how to open it the first time.
#   tools/package_mac.sh "Beta 2.5.3"
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:?version, e.g. \"Beta 2.5.3\"}"
APPNAME="Plants vs. Zombies DLC Edition"
BIN="$ROOT/recomp/build/play/pvzdlc_host"
SDL="$(readlink -f /opt/homebrew/opt/sdl2/lib/libSDL2-2.0.0.dylib)"
OUT="$ROOT/release/mac"
APP="$OUT/$APPNAME.app"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
# (without its debug map: the build machine's object file paths)
strip -S -x -o "$APP/Contents/MacOS/pvzdlc" "$BIN"
cp "$SDL" "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
chmod u+w "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
install_name_tool -id @rpath/libSDL2-2.0.0.dylib "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
install_name_tool -change /opt/homebrew/opt/sdl2/lib/libSDL2-2.0.0.dylib @rpath/libSDL2-2.0.0.dylib "$APP/Contents/MacOS/pvzdlc"
install_name_tool -add_rpath @executable_path/../Frameworks "$APP/Contents/MacOS/pvzdlc"
if otool -L "$APP/Contents/MacOS/pvzdlc" | grep -q /opt/homebrew; then echo "still linked to Homebrew:"; otool -L "$APP/Contents/MacOS/pvzdlc"; exit 1; fi

rsync -a --exclude .DS_Store --exclude '*.off' "$ROOT/gamedata/wide/" "$APP/Contents/Resources/wide/"

# the icon: icons/pc_icon.png (the user's round PvZ DLC badge, 256 x 256) on the macOS grid (824 of 1024), at the sizes
# it has (Finder scales the 256 one up for larger views)
ICONSET="$TMP/icon.iconset"
mkdir -p "$ICONSET"
for s in 16 32 128 256; do
    c=$((s * 824 / 1024))
    magick "$ROOT/icons/pc_icon.png" -resize ${c}x${c} -background none -gravity center -extent ${s}x${s} "$ICONSET/icon_${s}x${s}.png"
    if [ $s -le 128 ]; then
        magick "$ROOT/icons/pc_icon.png" -resize $((c * 2))x$((c * 2)) -background none -gravity center \
            -extent $((s * 2))x$((s * 2)) "$ICONSET/icon_${s}x${s}@2x.png"
    fi
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/icon.icns"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>$APPNAME</string>
    <key>CFBundleDisplayName</key><string>Plants vs. Zombies: DLC Edition</string>
    <key>CFBundleIdentifier</key><string>io.github.aks796.pvzdlc</string>
    <key>CFBundleExecutable</key><string>pvzdlc</string>
    <key>CFBundleIconFile</key><string>icon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$(date +%Y%m%d%H%M)</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
EOF

codesign --force --deep --sign - "$APP"
codesign --verify --deep "$APP"
if strings -a "$APP/Contents/MacOS/pvzdlc" "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib" | grep -q "$HOME"; then echo "the app names $HOME"; exit 1; fi

mkdir -p "$OUT"
cp "$ROOT/release/README_mac.txt" "$OUT/README.txt"
cp -R "$ROOT/release/stage/licenses" "$OUT/"
cp "$ROOT/icons/pc_cover.png" "$OUT/cover.png"  # (a Steam library picture: README_mac.txt)
ZIP="$ROOT/release/PvZ_DLC_Edition_Mac_$(echo "$VERSION" | tr -d ' ').zip"
rm -f "$ZIP"
(cd "$OUT" && ditto -c -k --norsrc --keepParent "$APPNAME.app" "$TMP/app.zip" && zip -q -X -r "$TMP/app.zip" README.txt licenses cover.png -x '*.DS_Store')
mv "$TMP/app.zip" "$ZIP"
echo "built $ZIP"
