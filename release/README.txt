Plants vs. Zombies: DLC Edition (Nintendo Switch), Beta 2.4
===========================================================

A native Switch port of the fan-made Plants vs. Zombies: DLC Edition v2.7.2, with controller play
(in the style of the Xbox 360 version and PvZ Touch), touch screen support and 16:9 widescreen.

This is a beta: please report problems (with sd:/switch/pvzdlc/debug.log, see "verbose" below).
This package contains no game files. You need your own DLC Edition exe.


Changes in Beta 2.4
-------------------
- No DeltaPatcher needed: the port makes the DLC Edition exe itself from your own PlantsVsZombies.exe
  and the DLC team's patch for it (Steam / EA / OG Patch.xdelta), on the first launch.
- Versus: the hedge at the right of the lawn covers the target zombies as the edge of the PC
  version's screen does: each target's sign and the front of its head show, the same on every row
  and every map (on the pool maps some rows showed much more than others). The top zombie's head
  goes over the brains count, as on PC.
- Versus: the top bush no longer covers the brains count, and no longer disappears while the lawn
  pans at the start of a match.
- Versus: Woods: the tree trunk on the right stands a little further right, so the targets show.
- Versus: the Menu button of the plant select stays in the top-right corner (off the brains count)
  while the plant select slides in and out too.
- Versus: the shovel sits in the screen's bottom-left corner, off the bottom row's lawn mower.
- The DLC's credits roll names the port, after the DLC team.

Changes in Beta 2.3
-------------------
- The flashlight (Adventure 7-5 and the Lights Out mini-game) follows the controller cursor
  (before, it only followed touch).
- The right stick moves the flashlight on its own, faster than the cursor, to look around the
  lawn. It stays where you leave it until you move the cursor again.
- The darkness in those levels reaches the left edge of the screen during the opening pan to the
  zombies (a strip on the left was left undimmed).

Changes in Beta 2.2
-------------------
- Conveyor belt levels: planting a seed packet that isn't the leftmost one now uses up that packet
  (before, the packet to its left disappeared and the chosen one stayed on the belt).
- BOMB All Together!: the explosive plant limit (the BOMB sign) now applies to the controller too.
  With none left, planting an explosive plant plays the buzzer, as clicking its packet does.
- On the lawn the D-pad moves the cursor one square per press, at once (holding it repeats, as in the
  menus). The left stick still moves it freely in any direction.

Changes in Beta 2.1
-------------------
- Holding B digs up one plant at a time, as the PC shovel does: a plant first, then its pumpkin,
  then the flower pot or lily pad under it (before, it could take them all at once).
- Digging is a little quicker.

Changes in Beta 2
-----------------
- Level music builds up again: the drums and hi-hats come in as the zombies pile up, as on PC
  (in Beta 1 every level played at its horde intensity).
- The shovel can't be used before Crazy Dave hands it over in Adventure (holding B dug plants up
  early, breaking the Adventure's order).
- The tombstone's right edge on the main menu has its thick outline.


What you need
-------------
1. Your own Plants vs. Zombies (Game of the Year 1.0.0.1051, from Steam or the EA app, or the original)
   and the DLC team's DLC Edition v2.7.2 download, with its patches (Steam Patch.xdelta, EA Patch.xdelta,
   OG Patch.xdelta). The port applies the patch itself: DeltaPatcher isn't needed. Your PlantsVsZombies.exe is
   in your Plants vs. Zombies folder (Steam: right-click the game > Manage > Browse local files).
   An exe you already patched with DeltaPatcher works too (about 213 MB, any file name).
2. A Switch running custom firmware with the Homebrew Menu.
3. About 650 MB free on the SD card while installing: the patch and your exe, the DLC Edition exe made from them
   (deleted again), and about 230 MB the first launch unpacks.


Installing
----------
1. Copy the switch folder from this zip to the root of your SD card.
   You should now have sd:/switch/pvzdlc/pvzdlc.nro
2. Put your PlantsVsZombies.exe and the DLC's patch for it (or all three patches: the port finds the one that
   fits) in the same folder: sd:/switch/pvzdlc/. Or an exe you patched with DeltaPatcher instead.
3. Start the Homebrew Menu with full memory: hold R while you launch any game, then open
   "Plants vs. Zombies: DLC Edition". (The album / applet mode doesn't have enough memory for the game.)

On the first launch the port makes the DLC Edition exe from your PlantsVsZombies.exe and the patch (as
DeltaPatcher does), then unpacks the game from it (main.pak, dlc_game.exe, bass.dll, properties/). This
takes a minute; later launches skip it. The port checks the result is the v2.7.2 build it was made for,
and says so if it isn't. After that the patches and exes aren't needed any more, and you can delete them
to free space. Keep them if you'd like to be able to unpack again.

Saves are kept in sd:/switch/pvzdlc/userdata/ (user1.dlc ...). Saves from the PC version can be
copied there.


Controls (controller 1; controller 2 is player 2 in versus)
---------------------------------------------------------
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
Touch screen: works as the mouse everywhere. Drag a seed onto the lawn to plant it.
Names: A (or a tap) on a name box opens the Switch keyboard.

Options (empty files in sd:/switch/pvzdlc/)
  4x3       play in the original 4:3 instead of widescreen
  verbose   write a detailed log to debug.log (for bug reports; makes the game a little slower)


Credits
-------
Plants vs. Zombies by PopCap Games / Electronic Arts. Buy the game to play this.
Plants vs. Zombies: DLC Edition by Samen and the DLC team (Seeds community Discord: https://discord.gg/rTz2QHD9aR).
Widescreen backgrounds: HenryJk's PvZ widescreen mod, GPL-3 (licence: licenses/HENRYJK_LICENSE.txt).
Lawn bushes: PvZ Touch / TV (PopCap / EA).
Unpacking of the Enigma Virtual Box exe follows evbunpack by mos9527 (Apache 2.0).
MO3 music: libopenmpt (BSD-3, licence: licenses/LIBOPENMPT_LICENSE.txt). Graphics: deko3d, libnx (devkitPro).
Switch port: aks796.

This is an unofficial fan project. It is not affiliated with or endorsed by PopCap Games,
Electronic Arts or Nintendo.
