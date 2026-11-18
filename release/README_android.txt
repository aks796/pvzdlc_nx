Plants vs. Zombies: DLC Edition for Android, Beta 2.4
=====================================================

A native Android version of the fan-made Plants vs. Zombies: DLC Edition v2.7.2, made from the Switch port. It plays
by touch (the touch screen is the mouse, as on PC) in 16:9 widescreen, and a controller works too (in the style of
the Xbox 360 version and PvZ Touch). Versus can be played by two people on one screen, a finger each.

This is a beta, and the first Android build: please report problems (with debug.log, see "Bug reports" below).
This package contains no game files. You need your own DLC Edition exe.


What you need
-------------
1. A 64-bit (arm64) Android phone or handheld on Android 10 or newer, with OpenGL ES 3.0 (any phone from the
   last several years).
2. The DLC Edition v2.7.2 exe. The DLC team ships it as patches (xdelta) for your own copy of
   Plants vs. Zombies (1.0.0.1051, Game of the Year from Steam, or the EA app version).
   Patch your PlantsVsZombies.exe with DeltaPatcher as the DLC team's guide explains.
   The result is a single exe of about 213 MB. Any file name is fine. Copy it to your device.
3. About 700 MB free while installing (the app, the exe, and about 230 MB the first launch unpacks from it);
   about 300 MB after that.


Installing
----------
1. Open PvZ_DLC_Edition_Android_Beta2.4.apk on your device and allow installing apps from the app you opened it
   with, when Android asks. Updates install over it and keep your saves.
2. Start "PvZ DLC Edition". The first time, tap "Choose the exe" and pick your DLC Edition exe.
   The game is unpacked from it (a minute or less); after that the exe isn't needed and you can delete it.

The game folder is Android/data/io.github.aks796.pvzdlc/files on your device's storage (on Android 11 and newer,
most file managers can't open it on the device; connect the device to a computer to see it). Saves are kept in its
userdata/ folder (user1.dlc ...); saves from the PC version can be copied there. Uninstalling the app deletes them,
so copy them out first.


Controls
--------
Touch: as the mouse on PC. Tap to click; tap a seed packet and then the lawn to plant it, or drag the packet onto
the lawn. Tap a name box to type with the on-screen keyboard. Back works as Esc (the menu).

Versus on one screen: the plants player and the zombies player each use a finger, at the same time. In the draft
each taps their own cards (the zombies' cards are on the right). In the match the plants player drags seed packets
as usual; the zombies player drags zombie packets from their side of the seed bank onto the lawn right of the red
line, and touches that side of the lawn to collect brains. The zombies player's cursor only shows while they drag.

Controllers (built-in controls on handhelds, and Xbox, PlayStation, Switch Pro and most Bluetooth or USB
controllers): press a button and the controller plays, with its own cursor and button prompts; touch the screen and
touch plays again. The buttons on screen are named by their letters: A, B, X, Y as printed on Xbox controllers (on a PlayStation controller: Cross = A, Circle = B,
Square = X, Triangle = Y). L / R are the bumpers, ZL / ZR the triggers, + is Menu / Start and - is View / Back.

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
Versus: the first controller is player 1, the second player 2.


Options and bug reports
-----------------------
Options are empty files in the game folder:
  4x3       play in the original 4:3 instead of widescreen
  verbose   write a detailed log to debug.log (for bug reports; makes the game a little slower)
Bug reports: send debug.log from the game folder, with your device's name and Android version.


Credits
-------
Plants vs. Zombies by PopCap Games / Electronic Arts. Buy the game to play this.
Plants vs. Zombies: DLC Edition by Samen and the DLC team (Seeds community Discord: https://discord.gg/rTz2QHD9aR).
Widescreen backgrounds: HenryJk's PvZ widescreen mod, GPL-3 (licence: licenses/HENRYJK_LICENSE.txt).
Lawn bushes: PvZ Touch / TV (PopCap / EA).
Unpacking of the Enigma Virtual Box exe follows evbunpack by mos9527 (Apache 2.0).
MO3 music: libopenmpt (BSD-3, licence: licenses/LIBOPENMPT_LICENSE.txt). Window, sound, touch and controllers:
SDL2 (zlib).
Port: aks796.

This is an unofficial fan project. It is not affiliated with or endorsed by PopCap Games,
Electronic Arts, Google or Nintendo.
