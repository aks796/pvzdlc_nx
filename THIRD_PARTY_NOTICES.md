# Third-party notices

## Not included (bring your own)

- **Plants vs. Zombies**: © PopCap Games / Electronic Arts.
- **Plants vs. Zombies: DLC Edition v2.7.2**: by Samen and the DLC team. The port runs the player's own copy; the
  recompiler (`recomp/tools/x86recomp`) translates it at build time, and none of its code or files are in this
  repository.

## In the releases only (the art download and the packages)

- **PvZWidescreen** by HenryJk, GPL-3.0: the widescreen backgrounds and pieces (`gamedata/wide/hj_*`, the menu,
  shop, almanac and credits pictures made from them). The licence travels with them as `HENRYJK_LICENSE.txt`.
- **Lawn bushes** from Plants vs. Zombies Touch / TV (PopCap / EA): `gamedata/wide/tvbush/`.
- The port's own pictures (`port_art/`, `gamedata/wide/`) are made from the game's art by the scripts in `tools/`.

## Code in this repository

- **NFSMW-NX's SaltyNX template** (StevensND/nfsmw-nx, `extras/saltynx-template`), GPL-3.0:
  `recomp/host/saltynx.c` is a C port of it (the FPS block for the Status Monitor overlay on the Switch).
- **evbunpack** by mos9527 (github.com/mos9527/evbunpack), Apache-2.0: `recomp/host/evb_unpack.c` unpacks the Enigma
  Virtual Box exe and restores the game's exe as it does (the virtual file table's layout and the restore).
- **stb_vorbis** and **stb_image** by Sean Barrett (github.com/nothings/stb), public domain or MIT:
  `recomp/third_party/stb_vorbis` (its functions renamed `rtv_*`) and `recomp/third_party/stb_image.h`.

## Libraries the builds use (not in this repository)

- **libopenmpt 0.8.9** (lib.openmpt.org), BSD-3-Clause: the MO3 music. Its licence travels with the releases as
  `LIBOPENMPT_LICENSE.txt`.
- **SDL2 2.32.10** (libsdl.org), zlib: window, sound, input and touch on Mac, Windows and Android (the Mac app carries
  Homebrew's build, the Windows zip SDL2's own `SDL2.dll`, the Android app a build from source with its Java glue).
- **libnx** and **deko3d** (switchbrew, fincs), ISC / zlib: the Switch build.
- **llvm-mingw** (mstorsjo), the Windows build's compiler; its C++ runtime is linked into the exe (Apache-2.0 with
  LLVM exceptions).
