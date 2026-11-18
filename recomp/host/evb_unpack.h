/* evb_unpack.h: the game's files (dlc_game.exe, main.pak, bass.dll, properties/) from the DLC Edition's exe */
#pragma once
#include <stddef.h>

typedef void (*EvbProgress)(const char *what, int permille);

/* exe: the DLC Edition 2.7.2 exe (Enigma Virtual Box packed); dir: where its files go. 0 when done; else -1 and a
 * message for the player in err */
int evb_unpack(const char *exe, const char *dir, EvbProgress progress, char *err, size_t errlen);
