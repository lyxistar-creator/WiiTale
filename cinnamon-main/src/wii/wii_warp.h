#ifndef _BS_WII_WARP_H_
#define _BS_WII_WARP_H_

#include "common.h"
#include "runner.h"
#include "renderer.h"

// ===[ Room warp menu ]===
//
// A pointer-driven list of every room in the game, including the ones the player can
// never reach normally. Undertale has 336 of them, far more than one screen holds, so
// the list is paged.
//
// Opened and closed with 1 + Plus. While it is open the game is not stepped and the
// controller does not reach it, so the menu cannot be used by accident mid-battle.
//
//   aim + A     warp to the room under the cursor
//   d-pad L/R   previous / next page
//   B           close
void WiiWarp_init(Runner* runner);

// Reads the controller and acts on it. Call once per frame, after the input poll, so the
// pad state it sees is the current one.
void WiiWarp_update(Runner* runner);

bool WiiWarp_isOpen(void);
// Opened from the main menu rather than by a button combination.
void WiiWarp_open(Runner* runner);

// Draws the menu over the finished frame.
void WiiWarp_draw(Renderer* renderer, Runner* runner, int32_t gameW, int32_t gameH);

#endif /* _BS_WII_WARP_H_ */
