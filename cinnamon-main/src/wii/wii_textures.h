#ifndef _BS_WII_TEXTURES_H_
#define _BS_WII_TEXTURES_H_

#include "common.h"
#include <stdint.h>
#include <gccore.h>

// ===[ Wii texture page cache ]===
//
// Undertale ships 26 texture pages. Decoded to RGBA8 they total 188 MB, and even as
// 16bpp RGB5A3 they are 94 MB -- against 88 MB of total Wii RAM. Two things make it fit:
//
//  1. Pages are stored as CI8 (8bpp indices plus a 256-entry palette) wherever that is
//     lossless, which it is for most of Undertale's pixel art. The handful of pages that
//     would visibly band are kept as raw RGB5A3 instead.
//  2. Nothing is resident by default. Tiles are streamed from the pack on demand into a
//     fixed MEM2 budget and evicted least-recently-used.
//
// ===[ Why pages are split into tiles ]===
//
// GX cannot address a texture larger than 1024 texels in either axis; libogc's own
// header states the limit on GX_InitTexObj. GameMaker's atlas pages are up to 2048x2048,
// so anything packed beyond row or column 1024 is simply unreachable by the hardware and
// samples nonsense. The preprocessor therefore cuts every page into a grid of tiles of at
// most 1024x1024, and a sprite that spans more than one tile is drawn in pieces.
//
// Tiles of the same page share one palette, and therefore one hardware TLUT slot.

#define WII_MAX_TEXTURE_DIM 1024

// GX has 16 hardware TLUT slots for 256-entry palettes against 26 pages, so slots are
// tied to residency rather than to page identity: a page claims one while any of its
// tiles is resident and gives it back when the last one goes.
#define WII_MAX_TLUT_SLOTS 16

#define WII_TEX_PACK_MAGIC   0x57544558u  // 'WTEX'
#define WII_TEX_PACK_VERSION 2

// Tile storage format, carried in the tile's flags.
#define WII_TEXFMT_MASK   0x0001u
#define WII_TEXFMT_CI8    0x0000u
#define WII_TEXFMT_RGB5A3 0x0001u

// Opens the pack and reads its tables. Tile payloads stay on disk. budgetBytes is how
// much MEM2 to reserve for resident tiles. Returns false if the pack is missing or
// invalid, in which case the renderer still runs and simply draws nothing.
bool WiiTextures_init(const char* packPath, uint32_t budgetBytes);
void WiiTextures_destroy(void);

bool WiiTextures_isAvailable(void);

// LRU bookkeeping: call once per frame before any draw.
void WiiTextures_beginFrame(uint64_t frameCount);

// Full dimensions of an original atlas page, which is the space tpag coordinates live in.
uint16_t WiiTextures_getWidth(int32_t pageId);
uint16_t WiiTextures_getHeight(int32_t pageId);

// The tile grid covering a page. Returns false for an unknown or empty page.
bool WiiTextures_getGrid(int32_t pageId, int32_t* outCols, int32_t* outRows);

// Binds one tile, streaming it in and evicting as needed. Returns nullptr when the tile
// cannot be made resident. outX/outY receive the tile's origin in page coordinates and
// outW/outH its size, which is what turns a page-space rect into tile-space UVs.
GXTexObj* WiiTextures_getTile(int32_t pageId, int32_t col, int32_t row,
                              int32_t* outX, int32_t* outY, int32_t* outW, int32_t* outH);

int32_t  WiiTextures_getPageCount(void);

// ---- diagnostics, used by the on-screen inspector ----
int32_t  WiiTextures_enumerateResident(int32_t index, int32_t* outPageId,
                                       int32_t* outCol, int32_t* outRow);
bool     WiiTextures_isPagePaletted(int32_t pageId);
GXTexObj* WiiTextures_peekResident(int32_t index);

uint32_t WiiTextures_getResidentBytes(void);
uint32_t WiiTextures_getBudgetBytes(void);
int32_t  WiiTextures_getResidentCount(void);
uint32_t WiiTextures_getEvictionCount(void);
uint32_t WiiTextures_getStreamInCount(void);

#endif /* _BS_WII_TEXTURES_H_ */
