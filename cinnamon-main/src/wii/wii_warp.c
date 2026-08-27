#include "wii_warp.h"

#include "wii_renderer.h"
#include "wii_input.h"
#include "wii_saves.h"
#include "data_win.h"
#include "log.h"
#include "string_compat.h"

#include <gccore.h>
#include <wiiuse/wpad.h>

// ===[ Layout ]===
// Two columns, not the three this started with. A room name drawn at WII_UI_TEXT_SCALE is
// about twice as wide as the grid was first built for, and three to a row overlapped into
// an unreadable smear. Fewer rooms per page is the cost of being able to read any of them.
#define COLS 2
#define ROWS 10
#define PER_PAGE (COLS * ROWS)

#define MARGIN     10.0f
#define HEADER_H   34.0f
#define FOOTER_H   34.0f
#define CELL_PAD   2.0f

static bool    gOpen;
static int32_t gPage;
static bool    gPrevCombo;
static int32_t gHovered = -1;   // room index under the cursor, -1 when none

// Colours are BGR, the order GameMaker packs them in.
#define COL_PANEL   0x120C08
#define COL_BORDER  0xC0C0C0
#define COL_TEXT    0xFFFFFF
#define COL_HOVER   0x00D0FF   // amber
#define COL_CURRENT 0x40FF40   // green: the room being played right now

// ===[ Selecting without pointing ]===
//
// The grid was reachable only by aiming the remote at it. That is fine until the sensor
// bar is out of sight, or the remote is being held sideways to play, or the cursor is
// simply hard to follow against a dense grid of room names -- and it was the only way in.
//
// The d-pad now moves a selection through the grid, and running off an edge turns the
// page, so every room is reachable without pointing at all. Pointing still works and takes
// over the moment the remote is actually moved.
static int32_t gSelected;      // slot within the current page
static bool    gPointerActive; // true while the cursor is over a cell

// Where the cursor was last frame. Whether pointing wins is decided by whether the
// remote actually moved, not by whether it happens to be aimed at something: a remote
// resting on the sofa still points somewhere, and it would otherwise take the selection
// back from the d-pad the instant it was used.
static float gLastPx, gLastPy;
static bool  gHavePrevPointer;

void WiiWarp_init(Runner* runner) {
    (void) runner; // the overlay font is resolved lazily by the renderer
    gOpen = false;
    gPage = 0;
    gSelected = 0;
}

bool WiiWarp_isOpen(void) { return gOpen; }
void WiiWarp_open(Runner* runner) {
    gOpen = true;
    gPage = runner->currentRoomIndex / PER_PAGE;
    // Start on the room being played, so the d-pad has somewhere sensible to move from.
    gSelected = runner->currentRoomIndex % PER_PAGE;
    gPointerActive = false;
    gHavePrevPointer = false;
}

static int32_t pageCount(Runner* runner) {
    int32_t rooms = (int32_t) runner->dataWin->room.count;
    return (rooms + PER_PAGE - 1) / PER_PAGE;
}

// Geometry of one cell, in the game's 640x480 screen space.
static void cellRect(int32_t slot, int32_t gameW, int32_t gameH,
                     float* x0, float* y0, float* x1, float* y1) {
    float gridW = (float) gameW - MARGIN * 2.0f;
    float gridH = (float) gameH - HEADER_H - FOOTER_H;
    float cw = gridW / (float) COLS;
    float ch = gridH / (float) ROWS;

    int32_t col = slot % COLS;
    int32_t row = slot / COLS;

    *x0 = MARGIN + (float) col * cw + CELL_PAD;
    *y0 = HEADER_H + (float) row * ch + CELL_PAD;
    *x1 = *x0 + cw - CELL_PAD * 2.0f;
    *y1 = *y0 + ch - CELL_PAD * 2.0f;
}

static bool inside(float px, float py, float x0, float y0, float x1, float y1) {
    return px >= x0 && px <= x1 && py >= y0 && py <= y1;
}

void WiiWarp_update(Runner* runner) {
    // ---- open / close ----
    // 1 + Plus. Distinct from 1+2 (texture inspector) and Minus+Plus (pointer steering).
    bool combo = false;
    for (int chan = 0; chan < 4; chan++) {
        uint32_t h = WPAD_ButtonsHeld(chan);
        if ((h & WPAD_BUTTON_1) && (h & WPAD_BUTTON_PLUS)) combo = true;
    }
    if (combo && !gPrevCombo) {
        gOpen = !gOpen;
        if (gOpen) {
            // Open on the page holding the current room, which is nearly always where
            // the player wants to look first.
            gPage = runner->currentRoomIndex / PER_PAGE;
        }
        logInfo("WiiWarp: room menu %s\n", gOpen ? "open" : "closed");
    }
    gPrevCombo = combo;

    if (!gOpen) { gHovered = -1; return; }

    uint32_t down = 0;
    for (int chan = 0; chan < 4; chan++) down |= WPAD_ButtonsDown(chan);

    int32_t pages = pageCount(runner);
    int32_t rooms = (int32_t) runner->dataWin->room.count;

    if (down & WPAD_BUTTON_B) { gOpen = false; gHovered = -1; return; }

    // Plus and Minus still turn whole pages, for getting across 336 rooms quickly.
    if (down & WPAD_BUTTON_PLUS)  gPage = (gPage + 1) % pages;
    if (down & WPAD_BUTTON_MINUS) gPage = (gPage + pages - 1) % pages;

    // ---- the d-pad walks the grid ----
    //
    // Running off the left or right edge steps a page and re-enters from the other side,
    // so the whole list is one continuous run rather than a set of pages to be switched
    // between separately.
    int32_t dx = 0, dy = 0;
    if (down & WPAD_BUTTON_LEFT)  dx = -1;
    if (down & WPAD_BUTTON_RIGHT) dx = +1;
    if (down & WPAD_BUTTON_UP)    dy = -1;
    if (down & WPAD_BUTTON_DOWN)  dy = +1;

    if (dx != 0 || dy != 0) {
        gPointerActive = false;   // the d-pad was used last, so it owns the selection
        int32_t col = gSelected % COLS + dx;
        int32_t row = gSelected / COLS + dy;

        if (col < 0)     { col = COLS - 1; gPage = (gPage + pages - 1) % pages; }
        if (col >= COLS) { col = 0;        gPage = (gPage + 1) % pages; }
        if (row < 0)     row = ROWS - 1;
        if (row >= ROWS) row = 0;

        gSelected = row * COLS + col;

        // A page can end part way through the grid; do not park on an empty cell.
        while (gSelected > 0 && gPage * PER_PAGE + gSelected >= rooms) gSelected--;
    }

    // ---- what is under the cursor ----
    gHovered = -1;
    float px, py;
    if (WiiInput_getPointerRaw(&px, &py)) {
        const float MOVED = 4.0f;   // enough to ignore the hand's own shake
        if (gHavePrevPointer &&
            (px - gLastPx > MOVED || gLastPx - px > MOVED ||
             py - gLastPy > MOVED || gLastPy - py > MOVED)) {
            gPointerActive = true;
        }
        gLastPx = px; gLastPy = py;
        gHavePrevPointer = true;

        if (gPointerActive) {
            for (int32_t slot = 0; slot < PER_PAGE; slot++) {
                int32_t roomIndex = gPage * PER_PAGE + slot;
                if (roomIndex >= rooms) break;

                float x0, y0, x1, y1;
                cellRect(slot, 640, 480, &x0, &y0, &x1, &y1);
                if (inside(px, py, x0, y0, x1, y1)) { gHovered = roomIndex; break; }
            }
        }
    } else {
        gHavePrevPointer = false;   // sensor bar lost: the d-pad is all there is
        gPointerActive = false;
    }

    if (gHovered < 0 && !gPointerActive && gPage * PER_PAGE + gSelected < rooms) {
        gHovered = gPage * PER_PAGE + gSelected;
    }

    if ((down & WPAD_BUTTON_A) && gHovered >= 0) {
        // Warping leaves the game's plot flags describing a place the player is no longer
        // in, and saving from there is what eventually sends a file to the dog room. The
        // save cannot be made consistent from out here -- only the game's own scripts know
        // what each flag means -- so the next best thing is that it is never lost: a copy
        // goes to the automatic backup slot before every jump.
        WiiSaves_backupAuto();

        // Undertale deliberately lets a track play on across rooms of the same area, and
        // stops or changes it from script in the specific rooms where that should happen.
        // Warping jumps straight past that logic, so the previous room's music would keep
        // playing over the new one. Silencing everything first lets the destination start
        // its own music from its creation code; a room that only inherits music will be
        // quiet, which is the honest outcome rather than the wrong song.
        if (runner->audioSystem != nullptr) {
            runner->audioSystem->vtable->stopAll(runner->audioSystem);
        }

        // The same field room_goto sets. The main loop's room-change handling picks it up.
        runner->pendingRoom = gHovered;
        gOpen = false;
        logInfo("WiiWarp: warping to room %d (%s)\n", gHovered,
                runner->dataWin->room.rooms[gHovered].name != nullptr
                    ? runner->dataWin->room.rooms[gHovered].name : "?");
    }
}

static void drawLabel(Renderer* renderer, const char* text, float x, float y, uint32_t colour) {
    WiiRenderer_overlayText(renderer, text, x, y, colour, 1.0f, WII_UI_TEXT_SCALE);
}

void WiiWarp_draw(Renderer* renderer, Runner* runner, int32_t gameW, int32_t gameH) {
    if (!gOpen) return;

    DataWin* dw = runner->dataWin;
    int32_t rooms = (int32_t) dw->room.count;
    int32_t pages = pageCount(runner);



    WiiRenderer_beginOverlay(renderer, gameW, gameH);

    // Dim the game behind the panel rather than hiding it, so it is obvious the menu is
    // an overlay and the game is paused underneath.
    renderer->vtable->drawRectangle(renderer, 0, 0, (float) gameW, (float) gameH,
                                    COL_PANEL, 0.92f, false);

    char header[96];
    snprintf(header, sizeof(header), "ROOMS  %d-%d of %d   page %d/%d",
             gPage * PER_PAGE + 1,
             (gPage * PER_PAGE + PER_PAGE) < rooms ? gPage * PER_PAGE + PER_PAGE : rooms,
             rooms, gPage + 1, pages);
    drawLabel(renderer, header, MARGIN, 8.0f, COL_TEXT);

    for (int32_t slot = 0; slot < PER_PAGE; slot++) {
        int32_t roomIndex = gPage * PER_PAGE + slot;
        if (roomIndex >= rooms) break;

        float x0, y0, x1, y1;
        cellRect(slot, gameW, gameH, &x0, &y0, &x1, &y1);

        bool hovered = (roomIndex == gHovered);
        bool current = (roomIndex == runner->currentRoomIndex);
        uint32_t tint = hovered ? COL_HOVER : (current ? COL_CURRENT : COL_TEXT);

        if (hovered) {
            renderer->vtable->drawRectangle(renderer, x0, y0, x1, y1, COL_HOVER, 0.25f, false);
        }
        renderer->vtable->drawRectangle(renderer, x0, y0, x1, y1,
                                        hovered ? COL_HOVER : COL_BORDER, 0.7f, true);

        const char* name = dw->room.rooms[roomIndex].name;
        char label[64];
        snprintf(label, sizeof(label), "%d %s", roomIndex, name != nullptr ? name : "?");
        drawLabel(renderer, label, x0 + 4.0f, y0 + 3.0f, tint);
    }

    drawLabel(renderer, "d-pad: choose   +/-: page   A: warp   B: close",
              MARGIN, (float) gameH - FOOTER_H + 8.0f, COL_TEXT);

    WiiRenderer_endOverlay(renderer);

}
