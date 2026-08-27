#include "wii_frame.h"

#include "gettime.h"
#include "log.h"

#include <gccore.h>

// How far behind the schedule may drift before it is abandoned and restarted from now.
// Two frames is enough to absorb a single slow room transition without the pacer then
// trying to "catch up" through the frames that follow it.
#define RESYNC_SLACK_FRAMES 2.0

// An upper bound on how many vsyncs one frame may wait. At 30 Hz the normal answer is
// two; the guard only exists so that a bad target cannot wedge the loop.
#define MAX_VSYNC_WAITS 8

static double   gTarget = 30.0;      // frames per second
static double   gPeriod = 1.0 / 30.0;
static double   gNextFrameEnd;

static double   gWindowStart;        // start of the current one-second sample
static uint32_t gWindowFrames;
static double   gActualFps;

static uint32_t gLateFrames;
static uint64_t gTotalFrames;

static inline double nowSeconds(void) {
    return (double) nowNanos() / 1000000000.0;
}

void WiiFrame_init(double targetFps) {
    if (!(targetFps >= 1.0 && targetFps <= 240.0)) {
        logWarn("WiiFrame: implausible target %.2f fps, using 30\n", targetFps);
        targetFps = 30.0;
    }
    gTarget = targetFps;
    gPeriod = 1.0 / targetFps;

    double now = nowSeconds();
    gNextFrameEnd = now + gPeriod;
    gWindowStart = now;
    gWindowFrames = 0;
    gActualFps = 0.0;
    gLateFrames = 0;
    gTotalFrames = 0;

    logInfo("WiiFrame: locked to %.2f fps (%.2f ms per frame), fixed step\n",
            gTarget, gPeriod * 1000.0);
}

double WiiFrame_beginFrame(void) {
    // Always the same number, whatever the console actually managed. See the header for
    // why this is not the measured elapsed time.
    return gPeriod * 1000000.0;
}

void WiiFrame_endFrame(void) {
    gTotalFrames++;
    gWindowFrames++;

    double now = nowSeconds();

    if (now > gNextFrameEnd) {
        // The frame's work outlasted its budget; there is nothing to wait for.
        gLateFrames++;
        if (now > gNextFrameEnd + RESYNC_SLACK_FRAMES * gPeriod) {
            // Far enough behind that following the old schedule would mean a run of
            // zero-length frames. Start again from here instead.
            gNextFrameEnd = now;
        }
    } else {
        int waits = 0;
        while (nowSeconds() < gNextFrameEnd && waits++ < MAX_VSYNC_WAITS) {
            VIDEO_WaitVSync();
        }
    }

    // Advance the schedule by exactly one period, so the rate cannot drift the way it
    // does when each frame's deadline is measured from whenever the last one happened to
    // finish.
    gNextFrameEnd += gPeriod;

    double elapsed = now - gWindowStart;
    if (elapsed >= 1.0) {
        gActualFps = (double) gWindowFrames / elapsed;
        gWindowFrames = 0;
        gWindowStart = now;
    }
}

double   WiiFrame_targetFps(void)   { return gTarget; }
double   WiiFrame_actualFps(void)   { return gActualFps; }
uint32_t WiiFrame_lateFrames(void)  { return gLateFrames; }
uint64_t WiiFrame_totalFrames(void) { return gTotalFrames; }
