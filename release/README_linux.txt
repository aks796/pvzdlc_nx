Plants vs. Zombies: DLC Edition for Linux (widescreen and controller), Beta 2.5.3
=================================================================================

The fan-made Plants vs. Zombies: DLC Edition v2.7.2, native on Linux (no Wine or Proton), in 16:9 widescreen with
controller support, made from the Switch port. On a PC it plays as the PC version does, with the mouse and keyboard;
a controller works too (in the style of the Xbox 360 version and PvZ Touch). On a Steam Deck the controls play
first. It runs from your own DLC Edition exe, which it unpacks once.

This is a beta: please report problems (with debug.log from the game's folder, see "verbose" below).
This package contains no game files. You need your own DLC Edition exe.


Changes in Beta 2.5.3
---------------------
- Versus with controllers: Let's Rock shows its button, X (either player), once both sides have chosen their plants
  and zombies.
- Versus: + opens the menu during a match (the pause button by the clock; it did nothing before).
- Versus: the shovel shows B (held over a plant, it digs it up).
- Versus: at the end of a match, move a cursor onto the trophy to go on, as on PC; A also collects it, so nothing is
  left to touch.
- Cob Cannon: A on it picks it up, the cursor aims it, A fires and B puts it back with a controller (it couldn't be
  fired with one).
- Music: the original tunes' horde drums and hi-hats stay on the beat (they came in off it, at random: they ran at
  the wrong tempo).
- Versus: after a match, the Multiplayer screen works with a controller (only touch worked there, so it was stuck),
  its sides show the menu's art (they were white), and Back to Menu there leaves it (it came straight back).
- Versus: when the zombies win, the gravestones stay on the lawn while the view pans to the house (they were cut off
  at the old 4:3 edge, then gone).


Changes in Beta 2.5.2
---------------------
- Options: 3D Acceleration stays on (the port is made for the game's 3D drawing; turned off, the Switch version
  showed a wrong, stretched picture). A saved "off" is switched back on at the next start.


Changes in Beta 2.5.1
---------------------
- Y (fast forward), + (menu) and - (pause) on a controller press the keys the game itself is using. After a key was
  changed in the DLC's Change Keybinds, they still pressed the old one, so Y did nothing (keys named F1, Home, a
  number-pad key and so on work too).
- Moon stages: the blood moon's glow is the original's, a soft red glow round the moon (a dark disc sat round it),
  and its sky no longer shows faint horizontal lines.


Changes in Beta 2.5
-------------------
- The DLC's Keybind Hint (in its Settings) shows each seed packet's key on top of the packet, as on PC (the
  packets covered the keys). While a controller plays, the keys are hidden and the controller's button pictures
  show instead; they come back when you use the mouse.
- Crazy Dave says "click to continue" while the mouse plays, as on PC ("press A" with a controller).


What you need
-------------
1. A 64-bit Linux:
     x86_64 package: a PC or a Steam Deck with OpenGL 3.2 graphics drivers (any graphics card or chip of the last
     ten years, with Mesa or the vendor's driver), X11 or Wayland.
     arm64 package: an ARM board or handheld with OpenGL ES 3.0 (Raspberry Pi 4 / 5, Rockchip, ...).
   Any distribution from about 2020 on (glibc 2.31 or newer: Ubuntu 20.04, Debian 11, SteamOS 3, Fedora, Arch ...).
2. Your own Plants vs. Zombies (Game of the Year 1.0.0.1051, from Steam or the EA app, or the original)
   and the DLC team's DLC Edition v2.7.2 download, with its patches (Steam Patch.xdelta, EA Patch.xdelta,
   OG Patch.xdelta). The port applies the patch itself: DeltaPatcher isn't needed. Your PlantsVsZombies.exe is
   in your Plants vs. Zombies folder (Steam: right-click the game > Manage > Browse local files).
   An exe you already patched with DeltaPatcher works too (about 213 MB, any file name).
3. About 500 MB free: the game unpacks about 230 MB from the exe on the first launch.


Installing
----------
1. Extract the "Plants vs. Zombies DLC Edition" folder somewhere you can write to (your home folder: the game keeps
   its files and saves in this folder).
2. Copy your PlantsVsZombies.exe and the DLC's patch for it (or all three patches: the game finds the one that
   fits) into that folder, or choose them when the game asks (with zenity or kdialog installed, which most
   desktops and SteamOS have, a file chooser opens). An exe you patched with DeltaPatcher works too.
3. Start "pvzdlc" in the folder (double-click it, or ./pvzdlc in a terminal). The first launch makes the game from
   your exe and the patch and unpacks it (a few seconds); after that those files aren't needed any more.

Saves are kept in the folder's userdata/ folder (user1.dlc ...); saves from the PC version can be copied there.
This is a separate copy of the game: a DLC Edition you play through Wine or Proton isn't touched.

Steam and the Steam Deck: in Desktop Mode, add "pvzdlc" to Steam as a non-Steam game (Steam > Games > Add a
Non-Steam Game to My Library > Browse). Put the patch and your exe in the folder first: the file chooser isn't shown in Game Mode.
cover.png in the folder is a library picture for it (in Steam: right-click the game > Properties > the picture, or
Manage > Set custom artwork). In Game Mode the Deck's controls are player 1, as on the Switch, and its touch screen
works as the mouse.

Updating from an earlier version: extract the new version over the old folder and replace the files. Your saves
and the unpacked game stay, so nothing is patched or unpacked again.


Controls
--------
Mouse and keyboard: as in the PC version. Versus is set up in the game's Multiplayer screen, as on PC (one player
on the mouse, the other on the keyboard).

Controllers: press a button on one and the controller plays (with its own cursor and button prompts); move the
mouse and the mouse plays again. Xbox, PlayStation, Switch Pro and Steam Deck controls (and most others SDL knows)
work. The buttons on screen are named by their letters: A, B, X, Y as printed on Xbox and Steam Deck controls (on a
PlayStation controller: Cross = A, Circle = B, Square = X, Triangle = Y). L / R are the bumpers, ZL / ZR the
triggers, + is Menu / Start / Options and - is View / Back / Create.

Menus
  D-pad / left stick   move the selector        A   choose        B   back
  ZL / ZR              previous / next page (Almanac, Shop, letters, jukebox tracks)
  Right stick          scroll the Almanac's descriptions
Plant select
  D-pad / stick        move    A pick a card    B take back the last card    X  Let's Rock!
  ZL / ZR              page arrows    Y  Almanac    -  Shop    +  Menu
  L                    put every chosen card back    R  the Imitater (also: Right past the last card)
In a level
  D-pad                move the cursor one square    left stick  move it freely
                       (moving over sun and coins collects them)
  L / R                previous / next seed packet    A  plant    hold B  dig up a plant
  Y                    fast forward    ZL / ZR (held)  collect everything    +  menu    -  pause
  X                    the mini-game's own action (break a vase, Whack-a-Zombie ...)
  Right stick          move the flashlight on its own (Adventure 7-5, Lights Out)
Zen Garden
  D-pad / stick        move    A use the chosen tool    L / R  choose a tool    B  put it back
  ZR                   next garden    Y  Shop    +  Main Menu
Versus: a controller plays as its player's keyboard keys (set that player to the keyboard in the Multiplayer screen):
the first controller is player 1, the second player 2.

Full screen: F11.

Options (empty files in the game's folder)
  4x3       play in the original 4:3 instead of widescreen
  verbose   write a detailed log to debug.log (for bug reports; makes the game a little slower)


Credits
-------
Plants vs. Zombies by PopCap Games / Electronic Arts. Buy the game to play this.
Plants vs. Zombies: DLC Edition by Samen and the DLC team (Seeds community Discord: https://discord.gg/rTz2QHD9aR).
Widescreen backgrounds: HenryJk's PvZ widescreen mod, GPL-3 (licence: licenses/HENRYJK_LICENSE.txt).
Lawn bushes: PvZ Touch / TV (PopCap / EA).
Unpacking of the Enigma Virtual Box exe follows evbunpack by mos9527 (Apache 2.0).
MO3 music: libopenmpt (BSD-3, licence: licenses/LIBOPENMPT_LICENSE.txt). Window, sound and controllers: SDL2 (zlib,
lib/libSDL2-2.0.so.0).
Port: aks796.

This is an unofficial fan project. It is not affiliated with or endorsed by PopCap Games,
Electronic Arts, Valve or Nintendo.
