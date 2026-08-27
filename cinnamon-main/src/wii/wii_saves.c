#include "wii_saves.h"

#include "wii_renderer.h"
#include "wii_input.h"
#include "log.h"
#include "utils.h"
#include "stdio_compat.h"
#include "string_compat.h"
#include <stdlib.h>
#include <sys/stat.h>

#include <gccore.h>
#include <wiiuse/wpad.h>

// The files that together make up a save. Undertale writes all of them next to the game;
// any that a given playthrough has not created yet are simply skipped.
static const char* SAVE_FILES[] = {
    "file0", "file8", "file9", "undertale.ini", "config.ini",
};
#define SAVE_FILE_COUNT (sizeof(SAVE_FILES) / sizeof(SAVE_FILES[0]))

#define SLOT_COUNT 3
#define AUTO_SLOT  SLOT_COUNT      // the automatic backup sits after the manual slots
#define ROW_COUNT  (SLOT_COUNT + 1)

// Columns: what can be done to a slot.
enum { ACT_LOAD = 0, ACT_SAVE, ACT_CLEAR, ACT_COUNT };

static char* gGameDir;
static bool  gOpen;
static bool  gPrevCombo;
static int32_t gHoverRow = -1, gHoverCol = -1;
static char  gStatus[96];

// Colours are BGR, the order GameMaker packs them in.
#define COL_PANEL   0x140E0A
#define COL_BORDER  0xB0B0B0
#define COL_TEXT    0xFFFFFF
#define COL_DIM     0x808080
#define COL_HOVER   0x00D0FF
#define COL_FULL    0x40FF40
#define COL_WARN    0x4040FF

// ===[ Choosing without pointing ]===
//
// Same reasoning as the room list: aiming was the only way to reach a cell, and the cursor
// was drawn underneath this panel anyway. The d-pad now walks the grid, and pointing takes
// over the moment the remote is actually moved rather than merely aimed somewhere.
static int32_t gSelRow, gSelCol;
static bool    gPointerActive;
static float   gLastPx, gLastPy;
static bool    gHavePrevPointer;

// The automatic backup can only be restored, never written or wiped, so the d-pad skips
// the cells that would do nothing.
static bool cellSelectable(int32_t row, int32_t col) {
    if (row < 0 || row >= ROW_COUNT || col < 0 || col >= ACT_COUNT) return false;
    return !(row == AUTO_SLOT && col != ACT_LOAD);
}

static void moveSelection(int32_t dRow, int32_t dCol) {
    for (int32_t guard = 0; guard < ROW_COUNT * ACT_COUNT; guard++) {
        gSelRow = (gSelRow + dRow + ROW_COUNT) % ROW_COUNT;
        gSelCol = (gSelCol + dCol + ACT_COUNT) % ACT_COUNT;
        if (cellSelectable(gSelRow, gSelCol)) return;
        // Landed on the backup's save or clear cell: keep going the same way.
        if (dRow == 0 && dCol == 0) return;
    }
}

// ===[ paths ]===

static char* joinPath(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* p = (char*) safeMalloc(la + lb + 1);
    memcpy(p, a, la);
    memcpy(p + la, b, lb + 1);
    return p;
}

// "<game>/slots/<n>/" -- the auto backup uses "auto" in place of a number.
static char* slotDir(int32_t slot) {
    char tail[24];
    if (slot == AUTO_SLOT) snprintf(tail, sizeof(tail), "slots/auto/");
    else                   snprintf(tail, sizeof(tail), "slots/%d/", slot + 1);
    return joinPath(gGameDir, tail);
}

static bool fileExists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return false;
    fclose(f);
    return true;
}

static bool copyOne(const char* srcPath, const char* dstPath) {
    FILE* in = fopen(srcPath, "rb");
    if (in == nullptr) return false;

    FILE* out = fopen(dstPath, "wb");
    if (out == nullptr) { fclose(in); return false; }

    // Save files are a couple of kilobytes, so one modest buffer is plenty.
    static uint8_t buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    fclose(out);
    return ok;
}

// Whether each slot has anything in it, worked out once instead of every frame.
//
// This used to be asked during the draw, for all four rows, on every frame: up to twenty
// file opens per frame against a FAT card over USB. That is what made the menu crawl. The
// answer only changes when a slot is written or cleared, so it is cached and refreshed at
// those moments.
static bool gSlotUsed[ROW_COUNT];

static bool slotUsedUncached(int32_t slot) {
    char* dir = slotDir(slot);
    bool used = false;
    for (size_t i = 0; i < SAVE_FILE_COUNT && !used; i++) {
        char* p = joinPath(dir, SAVE_FILES[i]);
        used = fileExists(p);
        free(p);
    }
    free(dir);
    return used;
}

static void refreshSlotUsage(void) {
    for (int32_t row = 0; row < ROW_COUNT; row++) gSlotUsed[row] = slotUsedUncached(row);
}

static bool slotUsed(int32_t slot) {
    if (slot < 0 || slot >= ROW_COUNT) return false;
    return gSlotUsed[slot];
}

static void ensureSlotDirs(void) {
    char* base = joinPath(gGameDir, "slots");
    mkdir(base, 0777);
    free(base);
    for (int32_t s = 0; s <= AUTO_SLOT; s++) {
        char* d = slotDir(s);
        // slotDir ends in '/', which mkdir does not want.
        size_t len = strlen(d);
        if (len > 0 && d[len - 1] == '/') d[len - 1] = '\0';
        mkdir(d, 0777);
        free(d);
    }
}

// Copies every save file that exists from one directory to another.
static int32_t copySaveSet(const char* fromDir, const char* toDir) {
    int32_t copied = 0;
    for (size_t i = 0; i < SAVE_FILE_COUNT; i++) {
        char* src = joinPath(fromDir, SAVE_FILES[i]);
        char* dst = joinPath(toDir, SAVE_FILES[i]);
        if (fileExists(src) && copyOne(src, dst)) copied++;
        free(src);
        free(dst);
    }
    return copied;
}

static void clearSlot(int32_t slot) {
    char* dir = slotDir(slot);
    for (size_t i = 0; i < SAVE_FILE_COUNT; i++) {
        char* p = joinPath(dir, SAVE_FILES[i]);
        remove(p);
        free(p);
    }
    free(dir);
}

void WiiSaves_init(const char* gameDir) {
    gGameDir = safeStrdup(gameDir);
    ensureSlotDirs();
    snprintf(gStatus, sizeof(gStatus), "%s", "ready");
    logInfo("WiiSaves: slot storage under %sslots/\n", gGameDir);
}

void WiiSaves_backupAuto(void) {
    if (gGameDir == nullptr) return;
    char* dir = slotDir(AUTO_SLOT);
    int32_t n = copySaveSet(gGameDir, dir);
    free(dir);
    if (n > 0) logInfo("WiiSaves: automatic backup taken (%d files)\n", n);
}

bool WiiSaves_isOpen(void) { return gOpen; }
void WiiSaves_open(void) {
    gOpen = true;
    refreshSlotUsage();
    // Start on the first slot's LOAD, which is what the menu is opened for most often.
    gSelRow = 0; gSelCol = ACT_LOAD;
    gPointerActive = false;
    gHavePrevPointer = false;
}

// ===[ layout ]===
#define MARGIN    14.0f
#define HEADER_H  48.0f
#define ROW_H     56.0f
#define LABEL_W   150.0f

static void cellRect(int32_t row, int32_t col, int32_t gameW,
                     float* x0, float* y0, float* x1, float* y1) {
    float actionsW = (float) gameW - MARGIN * 2.0f - LABEL_W;
    float cw = actionsW / (float) ACT_COUNT;
    *x0 = MARGIN + LABEL_W + (float) col * cw + 3.0f;
    *y0 = HEADER_H + (float) row * ROW_H + 3.0f;
    *x1 = *x0 + cw - 6.0f;
    *y1 = *y0 + ROW_H - 10.0f;
}

static bool inside(float px, float py, float x0, float y0, float x1, float y1) {
    return px >= x0 && px <= x1 && py >= y0 && py <= y1;
}

void WiiSaves_update(Runner* runner) {
    bool combo = false;
    for (int chan = 0; chan < 4; chan++) {
        uint32_t h = WPAD_ButtonsHeld(chan);
        if ((h & WPAD_BUTTON_MINUS) && (h & WPAD_BUTTON_1)) combo = true;
    }
    if (combo && !gPrevCombo) {
        gOpen = !gOpen;
        if (gOpen) {
            refreshSlotUsage();
            gSelRow = 0; gSelCol = ACT_LOAD;
            gPointerActive = false;
            gHavePrevPointer = false;
        }
        logInfo("WiiSaves: slot menu %s\n", gOpen ? "open" : "closed");
    }
    gPrevCombo = combo;

    if (!gOpen) { gHoverRow = gHoverCol = -1; return; }

    uint32_t down = 0;
    for (int chan = 0; chan < 4; chan++) down |= WPAD_ButtonsDown(chan);
    if (down & WPAD_BUTTON_B) { gOpen = false; return; }

    // ---- the d-pad walks the grid ----
    if (down & WPAD_BUTTON_UP)    { gPointerActive = false; moveSelection(-1, 0); }
    if (down & WPAD_BUTTON_DOWN)  { gPointerActive = false; moveSelection(+1, 0); }
    if (down & WPAD_BUTTON_LEFT)  { gPointerActive = false; moveSelection(0, -1); }
    if (down & WPAD_BUTTON_RIGHT) { gPointerActive = false; moveSelection(0, +1); }

    gHoverRow = gHoverCol = -1;

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
            for (int32_t row = 0; row < ROW_COUNT; row++) {
                for (int32_t col = 0; col < ACT_COUNT; col++) {
                    // The automatic backup is restore-only: writing to it by hand or
                    // wiping it would defeat the point of having a safety net.
                    if (!cellSelectable(row, col)) continue;

                    float x0, y0, x1, y1;
                    cellRect(row, col, 640, &x0, &y0, &x1, &y1);
                    if (inside(px, py, x0, y0, x1, y1)) { gHoverRow = row; gHoverCol = col; }
                }
            }
        }
    } else {
        gHavePrevPointer = false;   // sensor bar lost: the d-pad is all there is
        gPointerActive = false;
    }

    if (gHoverRow < 0 && !gPointerActive) {
        gHoverRow = gSelRow;
        gHoverCol = gSelCol;
    }

    if (!(down & WPAD_BUTTON_A) || gHoverRow < 0) return;

    char* dir = slotDir(gHoverRow);
    const char* what = (gHoverRow == AUTO_SLOT) ? "backup" : "slot";

    switch (gHoverCol) {
        case ACT_LOAD: {
            if (!slotUsed(gHoverRow)) {
                snprintf(gStatus, sizeof(gStatus), "%s %d is empty", what, gHoverRow + 1);
                break;
            }
            // Loading replaces the live save. Back it up first so one misclick cannot
            // throw away the run that is in progress.
            char* autoDir = slotDir(AUTO_SLOT);
            if (gHoverRow != AUTO_SLOT) copySaveSet(gGameDir, autoDir);
            free(autoDir);

            int32_t n = copySaveSet(dir, gGameDir);
            snprintf(gStatus, sizeof(gStatus), "loaded %s %d (%d files) - restart the game",
                     what, gHoverRow + 1, n);
            refreshSlotUsage();   // loading fills the automatic backup
            logInfo("WiiSaves: loaded %s %d (%d files)\n", what, gHoverRow + 1, n);
            break;
        }
        case ACT_SAVE: {
            int32_t n = copySaveSet(gGameDir, dir);
            snprintf(gStatus, sizeof(gStatus), "saved to slot %d (%d files)", gHoverRow + 1, n);
            refreshSlotUsage();
            logInfo("WiiSaves: saved to slot %d (%d files)\n", gHoverRow + 1, n);
            break;
        }
        case ACT_CLEAR: {
            clearSlot(gHoverRow);
            snprintf(gStatus, sizeof(gStatus), "cleared slot %d", gHoverRow + 1);
            logInfo("WiiSaves: cleared slot %d\n", gHoverRow + 1);
            refreshSlotUsage();
            break;
        }
        default: break;
    }
    free(dir);
}

void WiiSaves_draw(Renderer* renderer, Runner* runner, int32_t gameW, int32_t gameH) {
    if (!gOpen) return;

    WiiRenderer_beginOverlay(renderer, gameW, gameH);
    renderer->vtable->drawRectangle(renderer, 0, 0, (float) gameW, (float) gameH,
                                    COL_PANEL, 0.93f, false);

    WiiRenderer_overlayText(renderer, "SAVE SLOTS", MARGIN, 10.0f, COL_TEXT, 1.0f, WII_UI_TEXT_SCALE);
    WiiRenderer_overlayText(renderer,
        "loading replaces the live save - restart the game afterwards",
        MARGIN, 28.0f, COL_DIM, 1.0f, WII_UI_TEXT_SCALE);

    static const char* ACTION_NAMES[ACT_COUNT] = { "LOAD", "SAVE", "CLEAR" };

    for (int32_t row = 0; row < ROW_COUNT; row++) {
        bool isAuto = (row == AUTO_SLOT);
        bool used = slotUsed(row);

        char label[48];
        if (isAuto) snprintf(label, sizeof(label), "auto backup");
        else        snprintf(label, sizeof(label), "slot %d", row + 1);

        float ry = HEADER_H + (float) row * ROW_H + 12.0f;
        WiiRenderer_overlayText(renderer, label, MARGIN, ry,
                                used ? COL_FULL : COL_DIM, 1.0f, WII_UI_TEXT_SCALE);
        WiiRenderer_overlayText(renderer, used ? "in use" : "empty",
                                MARGIN, ry + 18.0f, COL_DIM, 1.0f, WII_UI_TEXT_SCALE);

        for (int32_t col = 0; col < ACT_COUNT; col++) {
            if (isAuto && col != ACT_LOAD) continue;

            float x0, y0, x1, y1;
            cellRect(row, col, gameW, &x0, &y0, &x1, &y1);

            bool hovered = (row == gHoverRow && col == gHoverCol);
            // An action that would do nothing is drawn dim, so it is obvious before
            // pressing rather than after.
            bool enabled = (col == ACT_SAVE) || used;
            uint32_t tint = !enabled ? COL_DIM : (hovered ? COL_HOVER
                                    : (col == ACT_CLEAR ? COL_WARN : COL_TEXT));

            if (hovered && enabled) {
                renderer->vtable->drawRectangle(renderer, x0, y0, x1, y1, COL_HOVER, 0.22f, false);
            }
            renderer->vtable->drawRectangle(renderer, x0, y0, x1, y1, tint, 0.75f, true);

            const char* text = isAuto ? "RESTORE" : ACTION_NAMES[col];
            WiiRenderer_overlayText(renderer, text, x0 + 10.0f, y0 + 12.0f, tint, 1.0f, WII_UI_TEXT_SCALE);
        }
    }

    WiiRenderer_overlayText(renderer, gStatus, MARGIN, (float) gameH - 42.0f, COL_TEXT, 1.0f, WII_UI_TEXT_SCALE);
    WiiRenderer_overlayText(renderer, "d-pad or aim, then A   B: close",
                            MARGIN, (float) gameH - 24.0f, COL_DIM, 1.0f, WII_UI_TEXT_SCALE);

    WiiRenderer_endOverlay(renderer);
}
