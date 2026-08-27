#include "wii_input.h"

#include "runner_keyboard.h"
#include "log.h"
#include "wii_renderer.h"
#include "data_win.h"
#include "instance.h"
#include "stb_ds.h"

#include <gccore.h>
#include <wiiuse/wpad.h>

// Every mapping resolves to one of these GML virtual keys: the game reads the keyboard
// and nothing else, so the whole input layer is a controller-to-key translation.
#define KEY_CONFIRM 'Z'
#define KEY_CANCEL  'X'
#define KEY_MENU    'C'

// Key state is kept per controller rather than merged, because each player's instance has
// to be able to hear its own pad. Indexed by GML key code, which is 0..255.
#define MAX_PADS 4
static bool gCur[MAX_PADS][GML_KEY_COUNT];
static bool gPrev[MAX_PADS][GML_KEY_COUNT];

// What reached the game's shared keyboard last frame, for edge detection.
static bool gSharedPrev[GML_KEY_COUNT];

static WiiPadStyle gPadStyle = WII_PAD_WIIMOTE_SIDEWAYS;
static bool        gShouldExit;

// Analogue sticks are treated as a d-pad; past this fraction of full deflection the axis
// counts as held. 0.5 keeps diagonals reachable without a mushy dead zone.
#define STICK_THRESHOLD 0.5f

static void press(int32_t pad, int32_t gmlKey) {
    if (pad < 0 || pad >= MAX_PADS) return;
    if (gmlKey < 0 || gmlKey >= GML_KEY_COUNT) return;
    gCur[pad][gmlKey] = true;
}

void WiiInput_init(void) {
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_ALL, 640, 480);
    gShouldExit = false;
    logInfo("WiiInput: initialised (style=%d)\n", (int) gPadStyle);
}

void WiiInput_destroy(void) { WPAD_Shutdown(); }

void WiiInput_setPadStyle(WiiPadStyle style) { gPadStyle = style; }
WiiPadStyle WiiInput_getPadStyle(void) { return gPadStyle; }
bool WiiInput_shouldExit(void) { return gShouldExit; }

// ===[ Pointer steering ]===
static bool    gPointerMode;
static bool    gPointerValid;
static float   gPointerX, gPointerY;
static bool    gRawPointerValid;
static float   gRawPointerX, gRawPointerY;
static bool    gPrevToggleCombo;
static bool    gGameInputEnabled = true;

bool WiiInput_isPointerMode(void) { return gPointerMode; }

bool WiiInput_getPointer(float* outX, float* outY) {
    if (!gPointerValid) return false;
    if (outX) *outX = gPointerX;
    if (outY) *outY = gPointerY;
    return true;
}

bool WiiInput_getPointerRaw(float* outX, float* outY) {
    if (!gRawPointerValid) return false;
    if (outX) *outX = gRawPointerX;
    if (outY) *outY = gRawPointerY;
    return true;
}

void WiiInput_setGameInputEnabled(bool enabled) { gGameInputEnabled = enabled; }

bool WiiInput_padKey(int32_t pad, int32_t key, bool pressedEdge) {
    if (pad < 0 || pad >= MAX_PADS) return false;
    if (key < 0 || key >= GML_KEY_COUNT) return false;
    if (!pressedEdge) return gCur[pad][key];
    return gCur[pad][key] && !gPrev[pad][key];
}

bool WiiInput_padReleased(int32_t pad, int32_t key) {
    if (pad < 0 || pad >= MAX_PADS) return false;
    if (key < 0 || key >= GML_KEY_COUNT) return false;
    return !gCur[pad][key] && gPrev[pad][key];
}

static int32_t gPlayerObject = -2;

static int32_t findPlayerObject(DataWin* dw) {
    if (dw == nullptr) return -1;
    static const char* NAMES[] = { "obj_mainchara", "obj_player", "objPlayer", "player" };
    for (size_t n = 0; n < sizeof(NAMES) / sizeof(NAMES[0]); n++) {
        for (uint32_t i = 0; i < dw->objt.count; i++) {
            const char* name = dw->objt.objects[i].name;
            if (name != nullptr && strcmp(name, NAMES[n]) == 0) return (int32_t) i;
        }
    }
    return -1;
}

static Instance* findPlayerInstance(Runner* runner, int32_t objectIndex) {
    if (objectIndex < 0) return nullptr;
    int32_t count = (int32_t) arrlen(runner->instances);
    for (int32_t i = 0; i < count; i++) {
        Instance* inst = runner->instances[i];
        if (inst == nullptr || inst->destroyed || !inst->active) continue;
        if (inst->objectIndex == objectIndex) return inst;
    }
    return nullptr;
}

// Presses the arrow keys that walk player one toward the cursor.
static bool steerTowardPointer(Runner* runner) {
    if (!gPointerValid) return false;

    if (gPlayerObject == -2) gPlayerObject = findPlayerObject(runner->dataWin);
    Instance* player = findPlayerInstance(runner, gPlayerObject);
    if (player == nullptr) return false;

    int32_t vx, vy, vw, vh, px, py, pw, ph;
    if (!WiiRenderer_getLastView(runner->renderer, &vx, &vy, &vw, &vh, &px, &py, &pw, &ph))
        return false;

    float roomX = (float) vx + (gPointerX - (float) px) * (float) vw / (float) pw;
    float roomY = (float) vy + (gPointerY - (float) py) * (float) vh / (float) ph;

    float dx = roomX - player->x;
    float dy = roomY - player->y;

    const float DEAD = 6.0f;
    bool moved = false;
    if (dx >  DEAD) { press(0, VK_RIGHT); moved = true; }
    if (dx < -DEAD) { press(0, VK_LEFT);  moved = true; }
    if (dy >  DEAD) { press(0, VK_DOWN);  moved = true; }
    if (dy < -DEAD) { press(0, VK_UP);    moved = true; }
    return moved;
}

bool WiiInput_debugComboHeld(void) {
    for (int chan = 0; chan < MAX_PADS; chan++) {
        uint32_t held = WPAD_ButtonsHeld(chan);
        if ((held & WPAD_BUTTON_1) && (held & WPAD_BUTTON_2)) return true;
    }
    return false;
}

// ===[ per-pad mapping ]===

static void mapWiimoteDpad(int32_t pad, uint32_t held) {
    if (gPadStyle == WII_PAD_WIIMOTE_SIDEWAYS) {
        // Held NES-style, the physical d-pad is a quarter turn clockwise from the screen.
        if (held & WPAD_BUTTON_RIGHT) press(pad, VK_UP);
        if (held & WPAD_BUTTON_LEFT)  press(pad, VK_DOWN);
        if (held & WPAD_BUTTON_UP)    press(pad, VK_LEFT);
        if (held & WPAD_BUTTON_DOWN)  press(pad, VK_RIGHT);
    } else {
        if (held & WPAD_BUTTON_UP)    press(pad, VK_UP);
        if (held & WPAD_BUTTON_DOWN)  press(pad, VK_DOWN);
        if (held & WPAD_BUTTON_LEFT)  press(pad, VK_LEFT);
        if (held & WPAD_BUTTON_RIGHT) press(pad, VK_RIGHT);
    }
}

static void mapWiimoteButtons(int32_t pad, uint32_t held) {
    if (held & WPAD_BUTTON_2) press(pad, KEY_CONFIRM);
    if (held & WPAD_BUTTON_1) press(pad, KEY_CANCEL);
    if (held & WPAD_BUTTON_A) press(pad, KEY_CONFIRM);
    if (held & WPAD_BUTTON_B) press(pad, KEY_CANCEL);
    if (held & WPAD_BUTTON_MINUS) press(pad, KEY_MENU);
    if (held & WPAD_BUTTON_PLUS)  press(pad, VK_ENTER);
}

static void mapStick(int32_t pad, const joystick_t* js) {
    if (js == nullptr || js->mag < STICK_THRESHOLD) return;
    // wiiuse reports magnitude plus an angle in degrees, 0 at the top, growing clockwise.
    float ang = js->ang;
    if (ang <  45.0f || ang >= 315.0f) press(pad, VK_UP);
    else if (ang < 135.0f)             press(pad, VK_RIGHT);
    else if (ang < 225.0f)             press(pad, VK_DOWN);
    else                               press(pad, VK_LEFT);
}

static void mapClassic(int32_t pad, uint32_t held, const joystick_t* js) {
    if (held & WPAD_CLASSIC_BUTTON_UP)    press(pad, VK_UP);
    if (held & WPAD_CLASSIC_BUTTON_DOWN)  press(pad, VK_DOWN);
    if (held & WPAD_CLASSIC_BUTTON_LEFT)  press(pad, VK_LEFT);
    if (held & WPAD_CLASSIC_BUTTON_RIGHT) press(pad, VK_RIGHT);
    if (held & WPAD_CLASSIC_BUTTON_A) press(pad, KEY_CONFIRM);
    if (held & WPAD_CLASSIC_BUTTON_B) press(pad, KEY_CANCEL);
    if (held & WPAD_CLASSIC_BUTTON_Y) press(pad, KEY_CANCEL);
    if (held & WPAD_CLASSIC_BUTTON_X) press(pad, KEY_MENU);
    if (held & WPAD_CLASSIC_BUTTON_MINUS) press(pad, KEY_MENU);
    if (held & WPAD_CLASSIC_BUTTON_PLUS)  press(pad, VK_ENTER);
    mapStick(pad, js);
}

void WiiInput_poll(Runner* runner) {
    WPAD_ScanPads();

    for (int32_t p = 0; p < MAX_PADS; p++) {
        memcpy(gPrev[p], gCur[p], sizeof(gCur[p]));
        memset(gCur[p], 0, sizeof(gCur[p]));
    }

    // Minus and Plus together toggles pointer steering. Both are bound on their own, so
    // while the pair is held they are withheld from the game.
    bool toggleCombo = false;
    for (int chan = 0; chan < MAX_PADS; chan++) {
        uint32_t h = WPAD_ButtonsHeld(chan);
        if ((h & WPAD_BUTTON_MINUS) && (h & WPAD_BUTTON_PLUS)) toggleCombo = true;
    }
    if (toggleCombo && !gPrevToggleCombo) {
        gPointerMode = !gPointerMode;
        logInfo("WiiInput: pointer steering %s\n", gPointerMode ? "ON" : "OFF");
    }
    gPrevToggleCombo = toggleCombo;

    gRawPointerValid = false;
    WPADData* ir = WPAD_Data(WPAD_CHAN_0);
    if (ir != nullptr && ir->err == WPAD_ERR_NONE && ir->ir.valid) {
        gRawPointerX = ir->ir.x;
        gRawPointerY = ir->ir.y;
        gRawPointerValid = true;
    }
    gPointerValid = gRawPointerValid && gPointerMode;
    gPointerX = gRawPointerX;
    gPointerY = gRawPointerY;

    for (int chan = 0; chan < MAX_PADS; chan++) {
        WPADData* data = WPAD_Data(chan);
        if (data == nullptr || data->err != WPAD_ERR_NONE) continue;

        uint32_t held = WPAD_ButtonsHeld(chan);
        // HOME is handled by the main menu; quitting is a choice there, so a stray
        // press can no longer throw away an unsaved run.
        if (toggleCombo) held &= ~(WPAD_BUTTON_MINUS | WPAD_BUTTON_PLUS);

        switch (data->exp.type) {
            case WPAD_EXP_NUNCHUK:
                // With a Nunchuk attached the remote must be upright, so the d-pad is read
                // straight whatever the configured style says.
                if (held & WPAD_BUTTON_UP)    press(chan, VK_UP);
                if (held & WPAD_BUTTON_DOWN)  press(chan, VK_DOWN);
                if (held & WPAD_BUTTON_LEFT)  press(chan, VK_LEFT);
                if (held & WPAD_BUTTON_RIGHT) press(chan, VK_RIGHT);
                if (held & WPAD_BUTTON_A) press(chan, KEY_CONFIRM);
                if (held & WPAD_BUTTON_B) press(chan, KEY_CANCEL);
                if (held & WPAD_NUNCHUK_BUTTON_Z) press(chan, KEY_CONFIRM);
                if (held & WPAD_NUNCHUK_BUTTON_C) press(chan, KEY_CANCEL);
                if (held & WPAD_BUTTON_MINUS) press(chan, KEY_MENU);
                if (held & WPAD_BUTTON_PLUS)  press(chan, VK_ENTER);
                mapStick(chan, &data->exp.nunchuk.js);
                break;

            case WPAD_EXP_CLASSIC:
                mapClassic(chan, held, &data->exp.classic.ljs);
                break;

            default:
                mapWiimoteDpad(chan, held);
                mapWiimoteButtons(chan, held);
                break;
        }
    }

    if (gPointerMode) steerTowardPointer(runner);

    // ---- feed the game's shared keyboard ----
    //
    // Any controller may drive the game, so every pad is merged into one keyboard.
    bool shared[GML_KEY_COUNT];
    memset(shared, 0, sizeof(shared));

    if (gGameInputEnabled) {
        for (int32_t p = 0; p < MAX_PADS; p++) {
            for (int32_t k = 0; k < GML_KEY_COUNT; k++) {
                if (gCur[p][k]) shared[k] = true;
            }
        }
    }

    for (int32_t k = 0; k < GML_KEY_COUNT; k++) {
        if (shared[k] && !gSharedPrev[k])      RunnerKeyboard_onKeyDown(runner->keyboard, k);
        else if (!shared[k] && gSharedPrev[k]) RunnerKeyboard_onKeyUp(runner->keyboard, k);
        gSharedPrev[k] = shared[k];
    }
}
