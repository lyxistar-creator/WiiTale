// WiiTale - GameMaker: Studio runner for the Nintendo Wii.
//
// Entry point and main loop. Mirrors the structure of the PS3 backend, which is the
// closest relative: both are PowerPC, big-endian, and stream their assets rather than
// loading them whole.

#include "data_win.h"
#include "vm.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "overlay_file_system.h"
#include "noop_audio_system.h"
#include "wii_audio_system.h"
#include "log.h"
#include "utils.h"
#include "gettime.h"

#include "wii_renderer.h"
#include "wii_textures.h"
#include "wii_input.h"
#include "wii_warp.h"
#include "wii_saves.h"
#include "wii_menu.h"
#include "wii_frame.h"

#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include <malloc.h>
#include <unistd.h>
#include <dirent.h>

#include <gccore.h>
#include <fat.h>
#include <ogc/lwp_watchdog.h>

// ===[ Video ]===
// The FIFO is the command buffer GX pulls from; 256 KB is the usual homebrew size and
// comfortably absorbs a 2D frame's worth of quads.
#define DEFAULT_FIFO_SIZE (256 * 1024)

static GXRModeObj* gRmode;
static void*       gFramebuffer[2];
static int         gFbIndex;
static void*       gFifo;

// How much MEM2 the texture cache may hold.
//
// This was 24 MB, and that is what made Deltarune's battles crawl to well under one frame
// a second: its texture pack is 40 MB and a fight draws from most of it at once, so every
// frame evicted tiles it needed again immediately and re-read them off the card. Undertale
// has the same shape of problem in a milder form -- 26 pages, 62 MB of pack.
//
// Asking for 40 MB turns eviction off rather than merely making it rarer. MEM2 is 64 MB
// and the audio cache takes its own ~10 MB slice before this one; WiiTextures_init clamps
// the request to whatever the arena actually has left, so asking for more than exists is
// safe and simply gets less.
#define TEXTURE_POOL_BYTES (40u * 1024u * 1024u)

// Undertale renders at 640x480 and every asset is authored for it.
#define GAME_WIDTH  640
#define GAME_HEIGHT 480

// Draws corner markers over the game so the mapping from game coordinates to the screen
// can be read off directly. Set to 0 once the geometry is settled.
#define WIITALE_CALIBRATION 0

static bool gShouldExit;

static void onResetPressed(uint32_t irq, void* ctx) {
    (void) irq; (void) ctx;
    gShouldExit = true;
}

static void onPowerPressed(void) {
    gShouldExit = true;
}

static void initVideo(void) {
    VIDEO_Init();
    gRmode = VIDEO_GetPreferredMode(nullptr);

    // Two framebuffers, flipped by hand each frame. MEM1 is the only memory the video
    // interface can scan out of, so these have to live there.
    gFramebuffer[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(gRmode));
    gFramebuffer[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(gRmode));
    gFbIndex = 0;

    VIDEO_Configure(gRmode);
    VIDEO_SetNextFramebuffer(gFramebuffer[gFbIndex]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (gRmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    // ---- GX bring-up ----
    gFifo = memalign(32, DEFAULT_FIFO_SIZE);
    memset(gFifo, 0, DEFAULT_FIFO_SIZE);
    GX_Init(gFifo, DEFAULT_FIFO_SIZE);

    GXColor background = { 0, 0, 0, 255 };
    GX_SetCopyClear(background, GX_MAX_Z24);

    GX_SetViewport(0.0f, 0.0f, (float) gRmode->fbWidth, (float) gRmode->efbHeight, 0.0f, 1.0f);
    GX_SetDispCopyYScale((float) gRmode->xfbHeight / (float) gRmode->efbHeight);
    GX_SetScissor(0, 0, gRmode->fbWidth, gRmode->efbHeight);
    GX_SetDispCopySrc(0, 0, gRmode->fbWidth, gRmode->efbHeight);
    GX_SetDispCopyDst(gRmode->fbWidth, gRmode->xfbHeight);
    GX_SetCopyFilter(gRmode->aa, gRmode->sample_pattern, GX_TRUE, gRmode->vfilter);
    GX_SetFieldMode(gRmode->field_rendering,
                    ((gRmode->viHeight == 2 * gRmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_CopyDisp(gFramebuffer[gFbIndex], GX_TRUE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    logInfo("WiiTale: video up, %dx%d (xfb %d)\n",
            gRmode->fbWidth, gRmode->efbHeight, gRmode->xfbHeight);
}

// Resolves the EFB into the back framebuffer and flips.
static void presentFrame(void) {
    GX_DrawDone();
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);

    gFbIndex ^= 1;
    GX_CopyDisp(gFramebuffer[gFbIndex], GX_TRUE);
    GX_Flush();

    VIDEO_SetNextFramebuffer(gFramebuffer[gFbIndex]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

// Diagnostics also go to a file on the card, because the on-screen console is painted
// over by GX as soon as the game starts drawing. The image can then be read back on the
// host, which is the only way to see anything the runner reports mid-frame.
FILE* gLogFp;

// Where that log lives, and whether to reopen it after every line.
//
// fflush is not enough: it pushes the bytes into libfat, but the directory entry keeps
// the old size until the file is closed, so a run that ends without a clean shutdown
// leaves a log that reads as empty however much was written to it. Closing and reopening
// after each line updates the entry every time. Far too slow to leave on once the game is
// drawing, so it is switched off when the main loop starts -- which is exactly when the
// log stops being about startup anyway.
static char* gLogPath;
static bool  gLogSyncEveryLine = true;

// A budget of lines to write the safe, slow way even after startup.
//
// Diagnostics that only fire once something is switched on arrive long after the cheap
// path has taken over, and would then be lost to anything but a clean shutdown -- which
// is precisely when someone is most likely to just turn the console off. A feature that
// wants its output to survive asks for a burst.
static int32_t gLogSyncBudget;

void PlatformLog_syncNextLines(int32_t lines) {
    if (lines > gLogSyncBudget) gLogSyncBudget = lines;
}

// The libogc console draws straight into a framebuffer, and this backend flips between
// two of them. Once the game is running, any line printed to stdout paints console text
// -- and, when the console scrolls, a full-screen wipe -- into the buffer that is about
// to be shown, which appears as a screen flashing for a single frame. So the console is
// switched off as soon as the main loop starts, and diagnostics continue to the SD log.
static bool gConsoleActive = true;

void platformLog(const logType type, const char* format, va_list va) {
    const char* prefix = "";
    switch (type) {
        case LOG_TYPE_NORMAL:                    break;
        case LOG_TYPE_WARNING: prefix = "Warning: "; break;
        case LOG_TYPE_ERROR:   prefix = "Error: ";   break;
        case LOG_TYPE_DEBUG:   prefix = "Debug: ";   break;
    }

    va_list copy;
    va_copy(copy, va);

    if (gConsoleActive) {
        if (prefix[0] != '\0') fputs(prefix, stdout);
        vfprintf(stdout, format, va);
    }

    if (gLogFp != nullptr) {
        if (prefix[0] != '\0') fputs(prefix, gLogFp);
        vfprintf(gLogFp, format, copy);
        fflush(gLogFp);

        if ((gLogSyncEveryLine || gLogSyncBudget > 0) && gLogPath != nullptr) {
            if (gLogSyncBudget > 0) gLogSyncBudget--;
            fclose(gLogFp);
            gLogFp = fopen(gLogPath, "a");
        }
    }
    va_end(copy);
}

// Looks for the game in the usual homebrew locations, in order of preference.
static const char* GAME_DIRS[] = {
    "sd:/apps/wiitale/",
    "sd:/wiitale/",
    "usb:/apps/wiitale/",
    "usb:/wiitale/",
};
#define GAME_DIR_COUNT (sizeof(GAME_DIRS) / sizeof(GAME_DIRS[0]))

// Lists what is actually mounted, so a failure to find the game says something more
// useful than "not here". Without this the only way to tell an unmounted card from a
// misplaced file is guesswork.
static void dumpMountDiagnostics(void) {
    static const char* ROOTS[] = { "sd:/", "usb:/", "fat:/", "/" };

    for (size_t i = 0; i < sizeof(ROOTS) / sizeof(ROOTS[0]); i++) {
        DIR* dir = opendir(ROOTS[i]);
        if (dir == nullptr) {
            logError("  %-7s cannot be opened\n", ROOTS[i]);
            continue;
        }
        logError("  %-7s contents:\n", ROOTS[i]);
        int shown = 0;
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr && shown < 12) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            logError("            %s%s\n", ent->d_name,
                     (ent->d_type == DT_DIR) ? "/" : "");
            shown++;
        }
        if (shown == 0) logError("            (empty)\n");
        closedir(dir);
    }
}

static char* findGameDir(void) {
    for (size_t i = 0; i < GAME_DIR_COUNT; i++) {
        size_t len = strlen(GAME_DIRS[i]);
        char* candidate = (char*) safeMalloc(len + strlen("data.win") + 1);
        strcpy(candidate, GAME_DIRS[i]);
        strcat(candidate, "data.win");

        FILE* f = fopen(candidate, "rb");
        free(candidate);
        if (f != nullptr) {
            fclose(f);
            return safeStrdup(GAME_DIRS[i]);
        }
    }
    return nullptr;
}

int main(int argc, char* argv[]) {
    (void) argc; (void) argv;

    initVideo();

    // Console output goes to the framebuffer so early failures are visible without a
    // USB Gecko. GX overwrites it as soon as the game starts drawing, which is fine:
    // by then anything worth reading has already been printed.
    console_init(gFramebuffer[gFbIndex], 20, 20,
                 gRmode->fbWidth, gRmode->xfbHeight,
                 gRmode->fbWidth * VI_DISPLAY_PIX_SZ);

    SYS_SetResetCallback(onResetPressed);
    SYS_SetPowerCallback(onPowerPressed);

    if (!fatInitDefault()) {
        logError("WiiTale: no FAT device found. Is the SD card inserted?\n");
        VIDEO_WaitVSync();
        sleep(5);
        return 1;
    }

    char* gameDir = findGameDir();
    if (gameDir == nullptr) {
        logError("WiiTale: data.win not found. Expected it in one of:\n");
        for (size_t i = 0; i < GAME_DIR_COUNT; i++) logError("  %s\n", GAME_DIRS[i]);
        logError("What is actually mounted:\n");
        dumpMountDiagnostics();
        sleep(30);
        return 1;
    }
    {
        gLogPath = (char*) safeMalloc(strlen(gameDir) + strlen("wiitale.log") + 1);
        strcpy(gLogPath, gameDir);
        strcat(gLogPath, "wiitale.log");
        gLogFp = fopen(gLogPath, "w");
    }

    logInfo("WiiTale: game directory is %s\n", gameDir);

    char* dataWinPath = (char*) safeMalloc(strlen(gameDir) + strlen("data.win") + 1);
    strcpy(dataWinPath, gameDir);
    strcat(dataWinPath, "data.win");

    // ===[ Parse data.win ]===
    // TXTR and AUDO are deliberately left unparsed: at 12 MB and 33 MB they are the two
    // largest chunks in the file, and both are streamed from their own packs instead.
    // Rooms are lazy for the same reason. This is the same shape the PS2 and PS3
    // backends use, and it is what keeps the parsed game inside MEM1.
    DataWinParserOptions options = {0};
    options.parseGen8 = true;
    options.parseOptn = true;
    options.parseLang = true;
    options.parseExtn = true;
    options.parseSond = true;
    options.parseAgrp = true;
    options.parseSprt = true;
    options.parseBgnd = true;
    options.parsePath = true;
    options.parseScpt = true;
    options.parseGlob = true;
    options.parseShdr = true;
    options.parseFont = true;
    options.parseTmln = true;
    options.parseObjt = true;
    options.parseRoom = true;
    options.parseTpag = true;
    options.parseCode = true;
    options.parseVari = true;
    options.parseFunc = true;
    options.parseStrg = true;
    options.parseTxtr = false;
    options.parseAudo = false;
    options.skipLoadingPreciseMasksForNonPreciseSprites = true;
    options.lazyLoadRooms = true;
    options.loadType = DATAWINLOADTYPE_LOAD_PER_CHUNK;

    logInfo("WiiTale: loading %s ...\n", dataWinPath);
    DataWin* dataWin = DataWin_parse(dataWinPath, options);

    Gen8* gen8 = &dataWin->gen8;
    logInfo("WiiTale: loaded \"%s\" (WAD version %u, GameMaker %u.%u.%u.%u)\n",
            gen8->name, gen8->wadVersion,
            dataWin->detectedFormat.major, dataWin->detectedFormat.minor,
            dataWin->detectedFormat.release, dataWin->detectedFormat.build);

    // ===[ Audio ]===
    // Created before the texture cache because both carve their pools out of the MEM2
    // arena, and the texture pool is the one that can usefully shrink: it evicts pages,
    // whereas a sound that will not fit its slot simply cannot play.
    AudioSystem* audioSystem = WiiAudioSystem_create(gameDir, dataWinPath);
    if (audioSystem == nullptr) {
        logWarn("WiiTale: audio failed to start, continuing without sound\n");
        audioSystem = (AudioSystem*) NoopAudioSystem_create();
    }

    // ===[ Texture pack ]===
    {
        char* packPath = (char*) safeMalloc(strlen(gameDir) + strlen("textures.wtex") + 1);
        strcpy(packPath, gameDir);
        strcat(packPath, "textures.wtex");
        if (!WiiTextures_init(packPath, TEXTURE_POOL_BYTES)) {
            logWarn("WiiTale: no texture pack, the game will run but draw nothing.\n");
            logWarn("WiiTale: generate it with tools/wiitale-preprocess.\n");
        }
        free(packPath);

    }

    VMContext* vm = VM_create(dataWin);

    OverlayFileSystem* overlayFs = OverlayFileSystem_create(gameDir, gameDir);
    Renderer* renderer = WiiRenderer_create(GAME_WIDTH, GAME_HEIGHT);
    if (renderer == nullptr) return 1;

    WiiInput_init();

    Runner* runner = Runner_create(dataWin, vm, renderer, (FileSystem*) overlayFs, audioSystem);
    runner->debugMode = false;

    WiiMenu_init();
    WiiSaves_init(gameDir);
    WiiWarp_init(runner);

    Runner_initFirstRoom(runner);

    logInfo("WiiTale: entering main loop\n");
    // From here the framebuffers belong to GX alone. Everything still reaches the SD log.
    gConsoleActive = false;

    // What the game itself asks to be stepped at. Undertale states 30 in every one of its
    // rooms; a GameMaker 2 game leaves that field at 0 and puts the rate in GEN8 instead,
    // which the parser copies into the rooms. Reading it rather than hardcoding 30 keeps
    // this correct for both.
    double targetFps = 30.0;
    if (dataWin->gen8.gms2FPS > 0.0f) {
        targetFps = (double) dataWin->gen8.gms2FPS;
    } else if (runner->currentRoom != nullptr && runner->currentRoom->speed > 0) {
        targetFps = (double) runner->currentRoom->speed;
    }
    WiiFrame_init(targetFps);

    // Startup is over -- and the rate it locked to is now on record. Stop paying a close
    // and reopen per line; a warning that fires every frame would otherwise cost a
    // directory write every frame.
    gLogSyncEveryLine = false;

    uint64_t frame = 0;

    while (!gShouldExit && !runner->shouldExit && !WiiInput_shouldExit() && !WiiMenu_wantsExit()) {
        RunnerKeyboard_beginFrame(runner->keyboard);
        RunnerGamepad_beginFrame(runner->gamepads);
        WiiInput_poll(runner);

        // The warp menu reads the pad after the poll has scanned it, and takes the
        // controller away from the game for as long as it is open.
        WiiMenu_update(runner);
        WiiWarp_update(runner);
        WiiSaves_update(runner);
        WiiInput_setGameInputEnabled(!WiiWarp_isOpen() && !WiiSaves_isOpen() && !WiiMenu_isOpen());

        // A constant step, not the measured one. Handing the runner wall-clock time makes
        // a room that is expensive to draw also run faster, so the pace of a scene would
        // depend on how hard it happened to be to render.
        runner->deltaTime = WiiFrame_beginFrame();

        // The game is held still while the warp menu is up: stepping it behind a modal
        // overlay would let timers, battles and cutscenes run on unattended.
        if (!WiiWarp_isOpen() && !WiiSaves_isOpen() && !WiiMenu_isOpen()) Runner_step(runner);

        // Audio keeps updating either way, so music does not stall while the menu is open.
        float dt = (float) (runner->deltaTime / 1000000.0);
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        runner->audioSystem->vtable->update(runner->audioSystem, dt);

        WiiRenderer_setFrameCount(renderer, frame);

        // Runner_drawViews scales every view's port by runner->displayScaleX/Y, but
        // nothing in the shared code ever assigns those fields: Runner_create callocs
        // them and leaves them at 0, so the port collapses to 0x0 and views render
        // nothing. Runner_computeViewDisplayScale is the function the mouse-mapping code
        // already uses for exactly this quantity, so it is called here each frame before
        // drawing. Without this the game only draws on screens that have views disabled.
        Runner_computeViewDisplayScale(runner, GAME_WIDTH, GAME_HEIGHT,
                                       &runner->displayScaleX, &runner->displayScaleY);

        Runner_drawPre(runner, GAME_WIDTH, GAME_HEIGHT);
        Runner_beginFrame(runner, GAME_WIDTH, GAME_HEIGHT,
                          GAME_WIDTH, GAME_HEIGHT, GAME_WIDTH, GAME_HEIGHT);
        Runner_drawViews(runner, GAME_WIDTH, GAME_HEIGHT, false);
        renderer->vtable->endFrameInit(renderer);
        Runner_drawPost(runner, GAME_WIDTH, GAME_HEIGHT);
        renderer->vtable->endFrameEnd(renderer);
        Runner_drawGUI(runner, GAME_WIDTH, GAME_HEIGHT, GAME_WIDTH, GAME_HEIGHT);

#if WIITALE_CALIBRATION
        WiiRenderer_drawCalibration(renderer, GAME_WIDTH, GAME_HEIGHT);
#endif

        WiiWarp_draw(renderer, runner, GAME_WIDTH, GAME_HEIGHT);
        WiiSaves_draw(renderer, runner, GAME_WIDTH, GAME_HEIGHT);
        WiiMenu_draw(renderer, runner, GAME_WIDTH, GAME_HEIGHT);

        // The aiming cursor, drawn after the menus rather than before them.
        //
        // It used to come first, which meant every menu painted its own panel straight
        // over the top of it: the one moment the cursor exists to be seen was the one
        // moment it was hidden. A cursor belongs above whatever it is aimed at.
        {
            float px, py;
            if ((WiiInput_isPointerMode() || WiiWarp_isOpen() || WiiSaves_isOpen() || WiiMenu_isOpen()) &&
                WiiInput_getPointerRaw(&px, &py)) {
                WiiRenderer_drawPointer(renderer, px, py, GAME_WIDTH, GAME_HEIGHT);
            }
        }

        // Hold 1+2 to inspect the texture cache. Drawn last so it covers the frame.
        if (WiiInput_debugComboHeld()) {
            WiiRenderer_drawPageOverlay(renderer, GAME_WIDTH, GAME_HEIGHT);
        }

        WiiRenderer_present(renderer);

        // Matching the PS3 backend: skip the flip on a room change so the transition
        // does not show a half-built frame.
        if (runner->pendingRoom == -1) presentFrame();
        Runner_handlePendingRoomChange(runner);

        // Hold the frame to its budget. Without this the loop runs at the video
        // interface's 60 Hz while the game expects 30, and everything happens twice as
        // fast as it should.
        WiiFrame_endFrame();

        frame++;
    }

    logInfo("WiiTale: shutting down\n");

    runner->audioSystem->vtable->destroy(runner->audioSystem);
    runner->audioSystem = nullptr;
    renderer->vtable->destroy(renderer);

    Runner_free(runner);
    OverlayFileSystem_destroy(overlayFs);
    VM_free(vm);
    DataWin_free(dataWin);
    WiiTextures_destroy();
    WiiInput_destroy();

    if (gLogFp != nullptr) { fclose(gLogFp); gLogFp = nullptr; }

    free(dataWinPath);
    free(gameDir);

    return 0;
}
