# The Linux builds' toolchain (tools/build_linux.sh): Debian 11 (glibc 2.31, the base of Steam's own "sniper" runtime,
# so the program starts on SteamOS and most distributions of the last few years), GCC 10 for arm64 and its cross GCC
# for x86_64, and the headers SDL2 is built against for both (Debian's multiarch -dev packages). Xvfb and Mesa's
# software OpenGL / OpenGL ES let the build be started and looked at without a screen.
#   docker build -t pvzdlc-linux tools/linux
FROM debian:bullseye
# (Debian 11 is past its end of life: its packages from Debian's archive server, which keeps them)
RUN printf 'deb http://archive.debian.org/debian bullseye main\ndeb http://archive.debian.org/debian-security bullseye-security main\n' \
      > /etc/apt/sources.list && rm -f /etc/apt/sources.list.d/* \
    && echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99archive
RUN dpkg --add-architecture amd64 && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential gcc-x86-64-linux-gnu g++-x86-64-linux-gnu cmake ninja-build pkg-config file ca-certificates \
      libwayland-bin wayland-protocols \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxinerama-dev libxfixes-dev \
      libxkbcommon-dev libwayland-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libasound2-dev \
      libpulse-dev libudev-dev libdbus-1-dev libdrm-dev libgbm-dev \
      libx11-dev:amd64 libxext-dev:amd64 libxrandr-dev:amd64 libxcursor-dev:amd64 libxi-dev:amd64 libxss-dev:amd64 \
      libxinerama-dev:amd64 libxfixes-dev:amd64 libxkbcommon-dev:amd64 libwayland-dev:amd64 libegl1-mesa-dev:amd64 \
      libgl1-mesa-dev:amd64 libgles2-mesa-dev:amd64 libasound2-dev:amd64 libpulse-dev:amd64 libudev-dev:amd64 \
      libdbus-1-dev:amd64 libdrm-dev:amd64 libgbm-dev:amd64 \
      xvfb xauth libgl1-mesa-dri libgles2 \
    && rm -rf /var/lib/apt/lists/*
