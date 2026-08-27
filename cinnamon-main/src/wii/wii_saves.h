#ifndef _BS_WII_SAVES_H_
#define _BS_WII_SAVES_H_

#include "common.h"
#include "runner.h"
#include "renderer.h"

// ===[ Save slots ]===
//
// Undertale keeps one save, spread over a handful of files next to the game: file0 holds
// the party state, file8 and file9 hold other checkpoints, undertale.ini carries the
// persistent flags, and config.ini the settings. There is no slot system.
//
// This adds three slots and an automatic backup, purely by copying those files around --
// nothing about the game is modified. That also makes the room warp menu safe to use:
// warping into an arbitrary room leaves the game's plot flags inconsistent with where the
// player actually is, which is what sends a save to the dog room. Every warp takes an
// automatic backup first, so a broken save is always one restore away from working.
//
//   Minus + 1   open / close
//   aim + A     act on the cell under the cursor
//   B           close
void WiiSaves_init(const char* gameDir);
void WiiSaves_update(Runner* runner);
bool WiiSaves_isOpen(void);
// Opened from the main menu rather than by a button combination.
void WiiSaves_open(void);
void WiiSaves_draw(Renderer* renderer, Runner* runner, int32_t gameW, int32_t gameH);

// Copies the live save files into the automatic backup slot. Called before a warp.
// Silent when there is nothing to back up yet.
void WiiSaves_backupAuto(void);

#endif /* _BS_WII_SAVES_H_ */
