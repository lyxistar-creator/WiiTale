#include "wii_menu.h"

#include "wii_renderer.h"
#include "wii_input.h"
#include "wii_saves.h"
#include "wii_warp.h"
#include "wii_frame.h"
#include "log.h"
#include "string_compat.h"
#include "stdio_compat.h"

#include <gccore.h>
#include <wiiuse/wpad.h>

enum { ITEM_RESUME = 0, ITEM_SAVES, ITEM_WARP, ITEM_QUIT, ITEM_COUNT };

static const char* ITEM_HINT[ITEM_COUNT] = {
    "back to the game",
    "three slots, plus the automatic backup",
    "jump to any room, including hidden ones",
    "return to the Homebrew Channel",
};

// The toggles carry their state in their own label, so the menu says what is on without
// needing a second column for it.
static void itemText(int32_t item, char* out, size_t outSize) {
    switch (item) {
        case ITEM_RESUME: snprintf(out, outSize, "RESUME"); break;
        case ITEM_SAVES:  snprintf(out, outSize, "SAVE SLOTS"); break;
        case ITEM_WARP:   snprintf(out, outSize, "ROOM WARP"); break;
        case ITEM_QUIT:   snprintf(out, outSize, "QUIT"); break;
        default:          snprintf(out, outSize, "?"); break;
    }
}

static bool gOpen;
static bool gPrevHome;
static bool gWantsExit;
static int32_t gHover = -1;

// Colours are BGR, the order GameMaker packs them in.
#define COL_PANEL  0x140E0A
#define COL_TEXT   0xFFFFFF
#define COL_DIM    0x909090
#define COL_HOVER  0x00D0FF
#define COL_QUIT   0x4040FF

#define MARGIN    40.0f
#define TITLE_Y   40.0f
#define FIRST_Y   104.0f
#define ROW_H     68.0f

void WiiMenu_init(void) {
    gOpen = false;
    gWantsExit = false;
}

bool WiiMenu_isOpen(void)    { return gOpen; }
bool WiiMenu_wantsExit(void) { return gWantsExit; }

static void itemRect(int32_t item, int32_t gameW,
                     float* x0, float* y0, float* x1, float* y1) {
    *x0 = MARGIN;
    *y0 = FIRST_Y + (float) item * ROW_H;
    *x1 = (float) gameW - MARGIN;
    *y1 = *y0 + ROW_H - 14.0f;
}

static bool inside(float px, float py, float x0, float y0, float x1, float y1) {
    return px >= x0 && px <= x1 && py >= y0 && py <= y1;
}

void WiiMenu_update(Runner* runner) {
    uint32_t held = 0, down = 0;
    for (int chan = 0; chan < 4; chan++) {
        held |= WPAD_ButtonsHeld(chan);
        down |= WPAD_ButtonsDown(chan);
    }

    bool home = (held & WPAD_BUTTON_HOME) || (held & WPAD_CLASSIC_BUTTON_HOME);
    if (home && !gPrevHome) {
        // Never open on top of one of the sub-menus: HOME there means "back to the game".
        if (WiiSaves_isOpen() || WiiWarp_isOpen()) gOpen = false;
        else gOpen = !gOpen;
        logInfo("WiiMenu: %s\n", gOpen ? "open" : "closed");
    }
    gPrevHome = home;

    if (!gOpen) { gHover = -1; return; }

    if (down & WPAD_BUTTON_B) { gOpen = false; return; }

    gHover = -1;
    float px, py;
    if (WiiInput_getPointerRaw(&px, &py)) {
        for (int32_t i = 0; i < ITEM_COUNT; i++) {
            float x0, y0, x1, y1;
            itemRect(i, 640, &x0, &y0, &x1, &y1);
            if (inside(px, py, x0, y0, x1, y1)) { gHover = i; break; }
        }
    }

    if (!(down & WPAD_BUTTON_A) || gHover < 0) return;

    switch (gHover) {
        case ITEM_RESUME: gOpen = false; break;
        case ITEM_SAVES:  gOpen = false; WiiSaves_open(); break;
        case ITEM_WARP:   gOpen = false; WiiWarp_open(runner); break;
        case ITEM_QUIT:   gWantsExit = true; break;
        default: break;
    }
}

void WiiMenu_draw(Renderer* renderer, Runner* runner, int32_t gameW, int32_t gameH) {
    if (!gOpen) return;

    WiiRenderer_beginOverlay(renderer, gameW, gameH);
    renderer->vtable->drawRectangle(renderer, 0, 0, (float) gameW, (float) gameH,
                                    COL_PANEL, 0.93f, false);

    WiiRenderer_overlayText(renderer, "WIITALE", MARGIN, TITLE_Y - 30.0f, COL_TEXT, 1.0f, WII_UI_TEXT_SCALE);
    WiiRenderer_overlayText(renderer, "point and press A", MARGIN, TITLE_Y, COL_DIM, 1.0f, WII_UI_TEXT_SCALE * 0.75f);

    for (int32_t i = 0; i < ITEM_COUNT; i++) {
        float x0, y0, x1, y1;
        itemRect(i, gameW, &x0, &y0, &x1, &y1);

        bool hovered = (i == gHover);
        uint32_t tint = hovered ? COL_HOVER : (i == ITEM_QUIT ? COL_QUIT : COL_TEXT);

        if (hovered) {
            renderer->vtable->drawRectangle(renderer, x0, y0, x1, y1, COL_HOVER, 0.20f, false);
        }
        renderer->vtable->drawRectangle(renderer, x0, y0, x1, y1, tint, 0.75f, true);

        char label[64];
        itemText(i, label, sizeof(label));
        WiiRenderer_overlayText(renderer, label, x0 + 14.0f, y0 + 8.0f, tint, 1.0f, WII_UI_TEXT_SCALE);
        WiiRenderer_overlayText(renderer, ITEM_HINT[i], x0 + 14.0f, y0 + 30.0f, COL_DIM, 1.0f, WII_UI_TEXT_SCALE * 0.6f);
    }

    // The frame rate, where it can actually be read. "Locked to 30" is a claim about the
    // console keeping up, and the only honest way to state it is to show the measured
    // rate next to the target and count the frames that missed.
    {
        char status[96];
        double actual = WiiFrame_actualFps();
        uint32_t late = WiiFrame_lateFrames();
        uint64_t total = WiiFrame_totalFrames();
        if (actual > 0.0) {
            snprintf(status, sizeof(status), "%.1f fps (target %.0f, %u late/%llu)",
                     actual, WiiFrame_targetFps(), late,
                     (unsigned long long) total);
        } else {
            snprintf(status, sizeof(status), "target %.0f fps, measuring...",
                     WiiFrame_targetFps());
        }
        WiiRenderer_overlayText(renderer, status, MARGIN, (float) gameH - 46.0f,
                                (late * 20u > total) ? COL_QUIT : COL_DIM, 1.0f, WII_UI_TEXT_SCALE * 0.6f);
    }

    WiiRenderer_overlayText(renderer, "HOME or B: back to the game",
                            MARGIN, (float) gameH - 24.0f, COL_DIM, 1.0f, WII_UI_TEXT_SCALE * 0.6f);

    WiiRenderer_endOverlay(renderer);
}
