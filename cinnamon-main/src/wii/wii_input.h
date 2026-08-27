#ifndef _BS_WII_INPUT_H_
#define _BS_WII_INPUT_H_

#include "common.h"
#include "runner.h"
#include <stdint.h>

// ===[ Wii input ]===
//
// Undertale only ever reads the keyboard, so every controller is translated into the
// same handful of GML virtual keys the PC build uses:
//
//   arrows  movement          Z  confirm      X  cancel      C  menu
//
// Three controller shapes are supported at once, and any of them may be picked up
// mid-game; whichever one the player touches drives the same key set.
typedef enum {
    WII_PAD_WIIMOTE_SIDEWAYS = 0,  // held NES-style, the d-pad is rotated 90 degrees
    WII_PAD_WIIMOTE_UPRIGHT,       // held vertically, with or without a Nunchuk
    WII_PAD_CLASSIC,               // Classic Controller / Classic Controller Pro
} WiiPadStyle;

void WiiInput_init(void);
void WiiInput_destroy(void);

// Sets how a bare Wiimote's d-pad is interpreted. Sideways is the default because it
// is the natural way to hold the remote for a 2D game.
void WiiInput_setPadStyle(WiiPadStyle style);
WiiPadStyle WiiInput_getPadStyle(void);

// Polls every connected controller and feeds the resulting key transitions into the
// runner's keyboard. Call once per frame, after RunnerKeyboard_beginFrame.
void WiiInput_poll(Runner* runner);

// True once the player has asked to quit (HOME on a Wiimote, or the Classic's HOME).
bool WiiInput_shouldExit(void);

// True while 1 and 2 are held together. The game never asks for both at once, so it is
// free to use as a diagnostic toggle.
bool WiiInput_debugComboHeld(void);

// ===[ Pointer steering ]===
//
// An optional way to walk: aim the Wii Remote and the player heads for the cursor. It is
// not path-finding -- there is no map of the room's collisions here, so the player walks
// straight at the target and will lean on a wall that is in the way, exactly as if the
// d-pad were being held. Toggled with Minus + Plus, which no part of the game asks for
// together, and the d-pad keeps working the whole time.
bool WiiInput_isPointerMode(void);

// Where the remote is aimed, in the game's 640x480 screen space. False when the sensor
// bar is not in view, which is also when pointer steering silently falls back to the d-pad.
bool WiiInput_getPointer(float* outX, float* outY);

// The aim regardless of whether pointer steering is switched on, for menus that are
// driven by the cursor whatever the play mode is.
bool WiiInput_getPointerRaw(float* outX, float* outY);

// Stops controller input reaching the game without stopping it being read. Used while an
// overlay menu owns the controller, so pressing A there cannot also confirm in-game.
void WiiInput_setGameInputEnabled(bool enabled);

// One pad's state, for the multi-player input override. `pressedEdge` asks for the
// this-frame transition rather than the held state.
bool WiiInput_padKey(int32_t pad, int32_t key, bool pressedEdge);
bool WiiInput_padReleased(int32_t pad, int32_t key);

#endif /* _BS_WII_INPUT_H_ */
