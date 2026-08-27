#ifndef _BS_WII_RENDERER_H_
#define _BS_WII_RENDERER_H_

#include "renderer.h"

// GXTexObj, for the raw-texture entry point below.
#include <gccore.h>

// ===[ Wii GX renderer ]===
//
// Implements the runner's Renderer vtable on top of libogc's GX, the Hollywood's
// fixed-function pipeline. GRRLIB is used only to bring up video and hand us a
// framebuffer; every draw below goes through GX directly, because GRRLIB's drawing API
// cannot express what GameMaker asks for (arbitrary separate blend factors, render
// targets, texture stages).
//
// Two hardware facts shape everything here:
//
//  * There are no programmable shaders. GX has the TEV, a fixed set of combiner
//    stages. Games that lean on GLSL cannot be reproduced; Undertale 1.08 barely uses
//    shaders, so the shader entry points below are honest no-ops that report
//    "unsupported" rather than pretending.
//  * Render targets are the EFB plus GX_CopyTex. The EFB is 640x528 at most, which is
//    exactly enough for Undertale's 640x480 application surface, so that surface is
//    left as the EFB itself rather than being given a texture of its own.
Renderer* WiiRenderer_create(int screenWidth, int screenHeight);

// Presents the finished frame and waits for vsync.
void WiiRenderer_present(Renderer* renderer);

// Frame counter, used by the texture cache for its LRU bookkeeping.
void WiiRenderer_setFrameCount(Renderer* renderer, unsigned long long frame);

// The view rectangle and screen viewport of the last view drawn this frame. Together they
// map a point on screen back into room coordinates, which is what the Wii Remote pointer
// needs to know where the player is aiming. Returns false before any view has been drawn.
bool WiiRenderer_getLastView(Renderer* renderer,
                             int32_t* viewX, int32_t* viewY, int32_t* viewW, int32_t* viewH,
                             int32_t* portX, int32_t* portY, int32_t* portW, int32_t* portH);

// Draws corner markers and a border around the game rectangle, using a projection set
// up here rather than whatever the runner last left bound. It answers where game
// coordinates actually land on the TV, which is otherwise guesswork on a display that
// overscans and a framebuffer whose height depends on the video mode.
//
//   red    top-left        green  top-right
//   blue   bottom-left     yellow bottom-right     white dot: centre
void WiiRenderer_drawCalibration(Renderer* renderer, int32_t gameW, int32_t gameH);

// Draws every resident texture page as a thumbnail grid, straight from the cache.
//
// This answers the question a log cannot answer quickly: is the page data itself intact?
// If a page looks like the artwork it should be, the fault is in how sprites sample it;
// if it looks like noise or flat colour, the fault is in the pack or the upload.
//
// Each cell is framed in green for a palettised page and magenta for a raw one, and
// carries a five-bit readout of its page id (bright square = 1, most significant first).
void WiiRenderer_drawPageOverlay(Renderer* renderer, int32_t gameW, int32_t gameH);

// Draws the aiming cursor for pointer steering, in the game's screen space. Without it
// the mode is unusable: there would be no way to see where the remote is aimed.
void WiiRenderer_drawPointer(Renderer* renderer, float x, float y,
                             int32_t gameW, int32_t gameH);

// Brackets an overlay drawn in the game's screen space with alpha blending on. Between
// these two calls the ordinary vtable draw calls (drawRectangle, drawText) work in a
// 0..gameW by 0..gameH coordinate system, whatever the game left bound.
void WiiRenderer_beginOverlay(Renderer* renderer, int32_t gameW, int32_t gameH);
void WiiRenderer_endOverlay(Renderer* renderer);

// A readable font for the overlay menus, chosen once and cached.
//
// Picking the first font with glyphs is not good enough: in Undertale that is index 0,
// fnt_wingdings, and every menu came out in Gaster's alphabet. Preference goes by name,
// smallest legible first, and symbol fonts are skipped when falling back.
// Returns -1 when the game has no usable font.
int32_t WiiRenderer_uiFontIndex(Renderer* renderer);

// How much bigger than its authored size the overlay draws the game's font.
//
// The fonts these games ship are 8 pixels tall, meant for a monitor a foot from your face.
// The same 640x480 stretched across a television, seen from a sofa, is a different problem:
// at their authored size the menus were reported as almost unreadable. Nothing here can be
// laid out without agreeing on this number, so it lives with the drawing code.
#define WII_UI_TEXT_SCALE 2.0f

// Draws one line of overlay text in the UI font, scaled. Saves and restores the draw
// state, so a menu cannot leak its colour or alignment into the game.
void WiiRenderer_overlayText(Renderer* renderer, const char* text,
                             float x, float y, uint32_t bgrColor, float alpha, float scale);

// Draws a rectangle of a texture this backend owns, rather than one of the game's atlas
// pages. Coordinates are whatever space is currently set up -- inside an overlay pass,
// that is screen space.
//
// The sprite path cannot serve this: it takes a tpag index and looks the page up in the
// streaming cache, so a texture that never came from the game has no way in. Kris is the
// one caller.
void WiiRenderer_drawRawTexture(Renderer* renderer, GXTexObj* tex,
                                int32_t texW, int32_t texH,
                                int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                                float dstX, float dstY, float dstW, float dstH,
                                uint32_t bgrColor, float alpha);

#endif /* _BS_WII_RENDERER_H_ */
