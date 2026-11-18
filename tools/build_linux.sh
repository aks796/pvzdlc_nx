#!/bin/bash
# Builds the Linux program in Docker (the image of tools/linux/Dockerfile, made on the first run):
#   tools/build_linux.sh x86_64      -> recomp/build/linux/pvzdlc        (desktop OpenGL; PCs, the Steam Deck)
#   tools/build_linux.sh arm64       -> recomp/build/linux-arm64/pvzdlc  (OpenGL ES 3.0; ARM boards and handhelds)
# SDL2 2.32.10 is built from its source (TOOLCHAIN/SDL2-2.32.10, as for Android) for that architecture once, into
# recomp/build/<target>/sdl/prefix: X11, Wayland, KMS/DRM, PulseAudio and ALSA loaded when they're there, so it needs
# nothing but the C library to start. Then recomp: make TARGET=linux / linux-arm64. tools/package_linux.sh packs it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${1:?x86_64 or arm64}"
TC="${TOOLCHAIN:-$HOME/sixtyfour/toolchain}"  # (TOOLCHAIN=... for another folder)
case "$ARCH" in
x86_64) T=linux; XP=x86_64-linux-gnu-; MA=x86_64-linux-gnu; PROC=x86_64 ;;
arm64) T=linux-arm64; XP=; MA=aarch64-linux-gnu; PROC=aarch64 ;;
*) echo "x86_64 or arm64"; exit 1 ;;
esac
docker image inspect pvzdlc-linux >/dev/null 2>&1 || docker build -t pvzdlc-linux "$ROOT/tools/linux"
# (the project and SDL's source at fixed paths in the container, so no folder of this machine's goes into the program)
docker run --rm -v "$ROOT":/src -v "$TC/SDL2-2.32.10":/sdl:ro -w /src/recomp -e T="$T" -e XP="$XP" -e MA="$MA" \
    -e PROC="$PROC" pvzdlc-linux bash -c '
set -e
B=build/$T/sdl
if [ ! -f $B/prefix/lib/libSDL2-2.0.so.0 ]; then
    mkdir -p $B
    export PKG_CONFIG_LIBDIR=/usr/lib/$MA/pkgconfig:/usr/share/pkgconfig
    cmake -G Ninja -S /sdl -B $B/cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=$PROC -DCMAKE_C_COMPILER=${XP}gcc -DCMAKE_LIBRARY_ARCHITECTURE=$MA \
        -DCMAKE_INSTALL_PREFIX=/src/recomp/$B/prefix -DCMAKE_C_FLAGS="-ffile-prefix-map=/sdl=SDL2" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_WAYLAND_LIBDECOR=OFF -DSDL_PIPEWIRE=OFF \
        -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_NAS=OFF -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_FUSIONSOUND=OFF \
        -DSDL_LIBSAMPLERATE=OFF -DSDL_RPI=OFF -DSDL_VULKAN=OFF > $B.log 2>&1 || { tail -30 $B.log; exit 1; }
    cmake --build $B/cmake >> $B.log 2>&1 && cmake --install $B/cmake >> $B.log 2>&1 || { tail -30 $B.log; exit 1; }
    ${XP}strip --strip-unneeded $B/prefix/lib/libSDL2-2.0.so.0
    grep -E "X11|Wayland|KMSDRM|Pulse|ALSA|OpenGL" $B/cmake/CMakeCache.txt | grep -E "^SDL_(X11|WAYLAND|KMSDRM|PULSEAUDIO|ALSA|OPENGL|OPENGLES):BOOL" || true
fi
make TARGET=$T -j8
'
