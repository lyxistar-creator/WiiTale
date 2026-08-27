#ifndef _BS_WII_FRAME_H_
#define _BS_WII_FRAME_H_

#include "common.h"

// ===[ Fixed-rate frame pacing ]===
//
// Both games this runner targets are authored for 30 steps per second. Undertale states
// it in every one of its 336 rooms; Deltarune leaves the per-room field at 0 and puts
// 30.0 in GEN8 instead, the way GameMaker 2 does. Neither is a 60 Hz game, so running the
// step at the video interface's 60 Hz makes everything -- walking, dialogue, battles --
// happen at double speed.
//
// Two separate things are wanted here, and they are easy to confuse:
//
//   Capped   the frame never arrives *early*. This is what waiting on vsync gives.
//   Fixed    the game is also told a constant amount of time passed, so a frame that
//            arrives *late* becomes slow motion rather than a jump.
//
// Only the second is what "locked to 30" usually means. Feeding the runner the measured
// wall-clock delta instead makes a heavy room speed the game up to compensate, so a
// scene's pace depends on how hard it happens to be to draw. A fixed step is also what
// the games were tested at on PC, so their timers and animations land where the authors
// put them.
//
// Falling behind is not corrected by running extra steps. Catching up that way costs
// more time than it saves and turns one slow frame into a spiral; the pacer resyncs
// instead and counts the miss, which is what the statistics below are for.

// Starts pacing at `targetFps`. Values outside a sane range fall back to 30.
void WiiFrame_init(double targetFps);

// Call once at the top of the loop. Returns the fixed step, in microseconds, to hand to
// the runner as its delta time.
double WiiFrame_beginFrame(void);

// Call once at the very end of the loop, after presenting. Waits out the rest of the
// frame's budget on vsync, which is cheaper than spinning and keeps the flip aligned to
// the display.
void WiiFrame_endFrame(void);

// What the loop is aiming for.
double WiiFrame_targetFps(void);

// What it is actually achieving, averaged over the last second. Zero until a second has
// gone by. This is the number worth trusting -- the target is only an intention.
double WiiFrame_actualFps(void);

// Frames that overran their budget since startup, and the total seen. A lock that is
// holding reports zero late frames; anything else says where the console ran out.
uint32_t WiiFrame_lateFrames(void);
uint64_t WiiFrame_totalFrames(void);

#endif /* _BS_WII_FRAME_H_ */
