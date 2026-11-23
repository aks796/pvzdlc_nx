#!/bin/bash
# Builds the Android app (arm64, Android 10+): release/PvZ_DLC_Edition_Android_<version>.apk
#   tools/build_android.sh "Beta 2.5.3" 28
# No Gradle: the SDK's own tools (aapt2, javac, d8, zipalign, apksigner) put together the manifest and Java in
# android/, SDL2's Java glue, the port's pictures (gamedata/wide -> assets/wide) and the native libraries:
#   libSDL2.so  SDL 2.32.10 from source (~/sixtyfour/toolchain), its own CMake with the NDK's toolchain file
#   libmain.so  the game (recomp: make TARGET=android), NDK r27d mounted from its dmg in ~/sixtyfour/toolchain
# It is signed with android/release.keystore (made on the first run; keep it: updates must carry the same key; its
# password is in android/keystore.pass). Never publish either file.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:?version name, e.g. \"Beta 2.5.3\"}"
CODE="${2:?version code (a whole number that grows with every release)}"
SDK="$HOME/Library/Android/sdk"
BT="$SDK/build-tools/34.0.0"
JAR="$SDK/platforms/android-34/android.jar"
TC="${TOOLCHAIN:-$HOME/sixtyfour/toolchain}"  # (TOOLCHAIN=... for another folder)
MNT=/tmp/pvz-ndk-r27d
NDK="$MNT/AndroidNDK13750724.app/Contents/NDK"
SDLSRC="$TC/SDL2-2.32.10"
B="$ROOT/recomp/build/android"
W="$B/apk"

if [ ! -d "$NDK" ]; then
    mkdir -p "$MNT"
    hdiutil attach -nobrowse -readonly -mountpoint "$MNT" "$TC/android-ndk-r27d-darwin.dmg" >/dev/null
fi

# SDL2
if [ ! -f "$B/sdl/libSDL2.so" ]; then
    cmake -G Ninja -S "$SDLSRC" -B "$B/sdl/cmake" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TEST=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_C_FLAGS="-ffile-prefix-map=$SDLSRC=SDL2" \
        -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$SDLSRC=SDL2" >/dev/null
    cmake --build "$B/sdl/cmake" -j8 >/dev/null
    cp "$B/sdl/cmake/libSDL2.so" "$B/sdl/libSDL2.so"
fi

# the game
make -C "$ROOT/recomp" TARGET=android NDK="$NDK" SDL2_SRC="$SDLSRC" -j8 | grep -v '^CC\|^CXX' || true
[ -f "$B/libmain.so" ] || { echo "no libmain.so"; exit 1; }

# the signing key (once)
if [ ! -f "$ROOT/android/release.keystore" ]; then
    head -c 24 /dev/urandom | base64 | tr -d '/+=' > "$ROOT/android/keystore.pass"
    keytool -genkeypair -keystore "$ROOT/android/release.keystore" -alias pvzdlc -keyalg RSA -keysize 2048 \
        -validity 20000 -storepass "$(cat "$ROOT/android/keystore.pass")" -keypass "$(cat "$ROOT/android/keystore.pass")" \
        -dname "CN=aks796" >/dev/null 2>&1
fi

rm -rf "$W"
mkdir -p "$W/resin" "$W/gen" "$W/classes" "$W/dex" "$W/assets" "$W/native/lib/arm64-v8a"
# the icon: icons/mobile.png (the user's phone icon) at each launcher size
for d in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
    mkdir -p "$W/resin/mipmap-${d%%:*}"
    magick "$ROOT/icons/mobile.png" -resize "${d##*:}x${d##*:}" "$W/resin/mipmap-${d%%:*}/ic_launcher.png"
done
rsync -a --exclude .DS_Store --exclude '*.off' "$ROOT/gamedata/wide/" "$W/assets/wide/"

"$BT/aapt2" compile --dir "$W/resin" -o "$W/res.zip"
"$BT/aapt2" link -I "$JAR" --manifest "$ROOT/android/AndroidManifest.xml" -o "$W/base.apk" --java "$W/gen" \
    --min-sdk-version 29 --target-sdk-version 34 --version-code "$CODE" --version-name "$VERSION" -A "$W/assets" "$W/res.zip"
find "$SDLSRC/android-project/app/src/main/java" "$ROOT/android/java" "$W/gen" -name '*.java' > "$W/sources.txt"
javac --release 8 -encoding UTF-8 -nowarn -cp "$JAR" -d "$W/classes" @"$W/sources.txt" 2>&1 | grep -v '^Note:' || true
"$BT/d8" --release --min-api 29 --lib "$JAR" --output "$W/dex" $(find "$W/classes" -name '*.class')

cp "$W/base.apk" "$W/unsigned.apk"
(cd "$W/dex" && zip -q "$W/unsigned.apk" classes.dex)
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" --strip-debug -o "$W/native/lib/arm64-v8a/libSDL2.so" "$B/sdl/libSDL2.so"
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" --strip-debug -o "$W/native/lib/arm64-v8a/libmain.so" "$B/libmain.so"
# (no build machine paths in what ships: SDL's asserts name their files, debug info its folders)
if strings -a "$W/native/lib/arm64-v8a/"*.so | grep -q "$HOME"; then echo "a library names $HOME"; exit 1; fi
(cd "$W/native" && zip -q -r "$W/unsigned.apk" lib)
"$BT/zipalign" -p -f 4 "$W/unsigned.apk" "$W/aligned.apk"
OUT="$ROOT/release/PvZ_DLC_Edition_Android_$(echo "$VERSION" | tr -d ' ').apk"
"$BT/apksigner" sign --v4-signing-enabled false --ks "$ROOT/android/release.keystore" --ks-pass "file:$ROOT/android/keystore.pass" --out "$OUT" "$W/aligned.apk"
"$BT/apksigner" verify "$OUT"
ls -la "$OUT"
