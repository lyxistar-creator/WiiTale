#ifndef _BS_WII_MENU_H_
#define _BS_WII_MENU_H_

#include "common.h"
#include "runner.h"
#include "renderer.h"

// ===[ Main menu ]===
//
// One entry point for everything this port adds, opened with HOME the way any Wii game
// does it. The features used to sit behind button combinations, which meant nobody found
// them: the save slots went unnoticed for a whole session because they were on Minus + 1.
// A menu that announces itself is worth more than a shortcut that does not.
//
// HOME no longer quits on its own; quitting is an entry here, which also stops a stray
// press from throwing away an unsaved run.
void WiiMenu_init(void);
void WiiMenu_update(Runner* runner);
void WiiMenu_draw(Renderer* renderer, Runner* runner, int32_t gameW, int32_t gameH);

bool WiiMenu_isOpen(void);

// True once the player has chosen to quit from the menu.
bool WiiMenu_wantsExit(void);

#endif /* _BS_WII_MENU_H_ */
