Plants vs. Zombies: DLC Edition for Windows (widescreen and controller), Beta 2.5
=================================================================================

The fan-made Plants vs. Zombies: DLC Edition v2.7.2 in 16:9 widescreen with controller support, made from the
Switch port. It plays as the PC version does, with the mouse and keyboard; a controller works too (in the style of
the Xbox 360 version and PvZ Touch). It runs from your own DLC Edition exe, which it unpacks once.

This is a beta: please report problems (with debug.log from the game's folder, see "verbose" below).
This package contains no game files. You need your own DLC Edition exe.


Changes in Beta 2.5
-------------------
- The DLC's Keybind Hint (in its Settings) shows each seed packet's key on top of the packet, as on PC (the
  packets covered the keys). While a controller plays, the keys are hidden and the controller's button pictures
  show instead; they come back when you use the mouse.
- Crazy Dave says "click to continue" while the mouse plays, as on PC ("press A" with a controller).


What you need
-------------
1. A 64-bit Windows 10 or 11 PC with OpenGL 3.2 graphics drivers (any graphics card or chip from the last ten years).
2. Your own Plants vs. Zombies (Game of the Year 1.0.0.1051, from Steam or the EA app, or the original)
   and the DLC team's DLC Edition v2.7.2 download, with its patches (Steam Patch.xdelta, EA Patch.xdelta,
   OG Patch.xdelta). The port applies the patch itself: DeltaPatcher isn't needed. Your PlantsVsZombies.exe is
   in your Plants vs. Zombies folder (Steam: right-click the game > Manage > Browse local files).
   An exe you already patched with DeltaPatcher works too (about 213 MB, any file name).
3. About 500 MB free: the game unpacks about 230 MB from the exe on the first launch.


Installing
----------
1. Unzip the "Plants vs. Zombies DLC Edition" folder somewhere you can write to (your Documents or Desktop, not
   Program Files: the game keeps its files and saves in this folder).
2. Copy your PlantsVsZombies.exe and the DLC's patch for it (or all three patches: the game finds the one that
   fits) into that folder, or choose them when the game asks. An exe you patched with DeltaPatcher works too.
3. Start "PvZ DLC Edition.exe". The app isn't signed, so Windows may show "Windows protected your PC": click
   More info, then Run anyway. The first launch makes the game from your exe and the patch and unpacks it (a few
   seconds); after that those files aren't needed any more.

Saves are kept in the folder's userdata/ folder (user1.dlc ...); saves from the PC version can be copied there.
This is a separate copy of the game: your normal DLC Edition install isn't touched.

Updating from an earlier version: unzip the new version over the old folder and replace the files. Your saves and
the unpacked game stay, so nothing is patched or unpacked again.


Controls
--------
Mouse and keyboard: as in the PC version. Versus is set up in the game's Multiplayer screen, as on PC (one player
on the mouse, the other on the keyboard).

Controllers: press a button on one and the controller plays (with its own cursor and button prompts); move the
mouse and the mouse plays again. Xbox, PlayStation and Switch Pro controllers (and most others Windows knows) work.
The buttons on
screen are named by their letters: A, B, X, Y as printed on Xbox and Switch controllers (on a PlayStation
controller: Cross = A, Circle = B, Square = X, Triangle = Y). L / R are the bumpers, ZL / ZR the triggers,
+ is Menu / Start / Options and - is View / Back / Create.

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

Steam (Big Picture, a controller on the TV): add "PvZ DLC Edition.exe" to Steam as a non-Steam game; cover.png is a library
picture for it (in Steam: right-click the game > Manage > Set custom artwork).

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
MO3 music: libopenmpt (BSD-3, licence: licenses/LIBOPENMPT_LICENSE.txt). Window, sound and controllers: SDL2 (zlib).
Port: aks796.

This is an unofficial fan project. It is not affiliated with or endorsed by PopCap Games,
Electronic Arts, Microsoft or Nintendo.
