#include "wii_textures.h"

#include "log.h"
#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include <malloc.h>
#include <ogc/cache.h>
#include <ogc/system.h>

// ===[ Pack byte order ]===
// The pack is big-endian to match the Broadway, so these are plain loads here. They stay
// explicit so the format keeps its meaning if the host tool ever runs somewhere else.
static inline uint16_t readU16BE(const uint8_t* p) {
    return (uint16_t) (((uint32_t) p[0] << 8) | (uint32_t) p[1]);
}

static inline uint32_t readU32BE(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
         | ((uint32_t) p[2] <<  8) |  (uint32_t) p[3];
}

// ===[ MEM2 block pool ]===
// Tiles are a power-of-two number of bytes (a full 1024x1024 CI8 tile is 1 MB, RGB5A3 is
// 2 MB, edge tiles are smaller), so a block bitmap with power-of-two aligned runs cannot
// fragment: an allocation either finds an aligned run or the pool is genuinely full.
#define POOL_BLOCK_SIZE (256u * 1024u)
#define POOL_MAX_BLOCKS 256u

typedef struct {
    uint16_t width, height;
    uint16_t flags;
    uint32_t dataOffset;
    uint32_t dataSize;

    int32_t  pageId;         // owning page, for palette and slot lookup
    void*    texels;         // MEM2, nullptr when not resident
    GXTexObj texObj;
    bool     resident;
    uint64_t lastUsedFrame;
} WiiTexTile;

typedef struct {
    uint16_t width, height;  // full page size, the space tpag coordinates use
    uint16_t cols, rows;
    uint16_t firstTile;
    uint16_t tlutIndex;

    int32_t  tlutSlot;       // hardware slot while any tile is resident, else -1
    int32_t  residentTiles;  // how many of this page's tiles are loaded
} WiiTexPage;

static uint8_t*   gPoolBase;
static uint32_t   gPoolBlockCount;
static int32_t    gBlockOwner[POOL_MAX_BLOCKS];  // tile index in the block, -1 = free

static FILE*      gPackFp;
static WiiTexPage* gPages;
static int32_t    gPageCount;
static WiiTexTile* gTiles;
static int32_t    gTileCount;

static uint16_t   gTlutCount;
static uint16_t*  gTlutData;
static GXTlutObj* gTlutObjs;
static int32_t    gTlutSlotOwner[WII_MAX_TLUT_SLOTS];  // page id holding the slot, -1 free
static bool       gInitialized;

static uint64_t   gCurrentFrame;
static uint32_t   gResidentBytes;
static int32_t    gResidentCount;
static uint32_t   gEvictions;
static uint32_t   gStreamIns;
static uint32_t   gForcedFlushes;

static inline uint32_t blocksFor(uint32_t bytes) {
    return (bytes + POOL_BLOCK_SIZE - 1u) / POOL_BLOCK_SIZE;
}

static inline uint32_t roundUpPow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

static int32_t poolFindRun(uint32_t n) {
    for (uint32_t start = 0; start + n <= gPoolBlockCount; start += n) {
        bool ok = true;
        for (uint32_t i = 0; i < n; i++) {
            if (gBlockOwner[start + i] != -1) { ok = false; break; }
        }
        if (ok) return (int32_t) start;
    }
    return -1;
}

static void poolMark(uint32_t start, uint32_t n, int32_t owner) {
    for (uint32_t i = 0; i < n; i++) gBlockOwner[start + i] = owner;
}

static void unloadTile(int32_t tileIndex) {
    if (tileIndex < 0 || tileIndex >= gTileCount) return;
    WiiTexTile* tile = &gTiles[tileIndex];
    if (!tile->resident) return;

    for (uint32_t i = 0; i < gPoolBlockCount; i++) {
        if (gBlockOwner[i] == tileIndex) gBlockOwner[i] = -1;
    }

    WiiTexPage* page = &gPages[tile->pageId];
    if (--page->residentTiles <= 0) {
        page->residentTiles = 0;
        // The last tile of the page went: give the palette slot back.
        if (page->tlutSlot >= 0 && page->tlutSlot < WII_MAX_TLUT_SLOTS) {
            gTlutSlotOwner[page->tlutSlot] = -1;
            page->tlutSlot = -1;
        }
    }

    tile->texels = nullptr;
    tile->resident = false;
    gResidentBytes -= roundUpPow2(tile->dataSize);
    gResidentCount--;
}

// Pages already drawn this frame are skipped unless allowInFlight: GX consumes the
// command FIFO asynchronously, so overwriting a tile's memory while queued draws still
// reference it makes the GPU read the new tile's texels for the old sprite.
static int32_t pickVictim(int32_t protectTile, bool allowInFlight) {
    int32_t victim = -1;
    uint64_t oldest = (uint64_t) -1;
    for (int32_t i = 0; i < gTileCount; i++) {
        if (!gTiles[i].resident || i == protectTile) continue;
        if (!allowInFlight && gTiles[i].lastUsedFrame == gCurrentFrame) continue;
        if (gTiles[i].lastUsedFrame < oldest) {
            oldest = gTiles[i].lastUsedFrame;
            victim = i;
        }
    }
    return victim;
}

static bool evictOne(int32_t protectTile) {
    int32_t victim = pickVictim(protectTile, false);

    if (victim < 0) {
        victim = pickVictim(protectTile, true);
        if (victim < 0) return false;

        GX_DrawDone();
        gForcedFlushes++;
        if (gForcedFlushes == 1 || (gForcedFlushes % 100) == 0) {
            logWarn("WiiTextures: pool oversubscribed, stalled the GPU to evict (%u times)\n",
                    gForcedFlushes);
        }
    }

    unloadTile(victim);
    gEvictions++;
    return true;
}

// Claims the hardware TLUT slot for a page, evicting another page's tiles if every slot
// is taken. Returns -1 only when nothing can be given up.
static int32_t acquirePageTlut(int32_t pageId) {
    WiiTexPage* page = &gPages[pageId];
    if (page->tlutSlot >= 0) return page->tlutSlot;

    int32_t slot = -1;
    for (int32_t i = 0; i < WII_MAX_TLUT_SLOTS; i++) {
        if (gTlutSlotOwner[i] == -1) { slot = i; break; }
    }

    if (slot < 0) {
        // Free a slot by unloading every tile of the least recently used page holding one.
        int32_t victimPage = -1;
        uint64_t oldest = (uint64_t) -1;
        for (int32_t i = 0; i < WII_MAX_TLUT_SLOTS; i++) {
            int32_t owner = gTlutSlotOwner[i];
            if (owner < 0 || owner == pageId) continue;
            for (int32_t t = 0; t < gPages[owner].cols * gPages[owner].rows; t++) {
                int32_t idx = gPages[owner].firstTile + t;
                if (!gTiles[idx].resident) continue;
                if (gTiles[idx].lastUsedFrame < oldest) {
                    oldest = gTiles[idx].lastUsedFrame;
                    victimPage = owner;
                }
            }
        }
        if (victimPage < 0) return -1;

        GX_DrawDone();
        gForcedFlushes++;
        WiiTexPage* vp = &gPages[victimPage];
        for (int32_t t = 0; t < vp->cols * vp->rows; t++) unloadTile(vp->firstTile + t);
        slot = -1;
        for (int32_t i = 0; i < WII_MAX_TLUT_SLOTS; i++) {
            if (gTlutSlotOwner[i] == -1) { slot = i; break; }
        }
        if (slot < 0) return -1;
    }

    // The palette must be in texture memory before any object referencing the slot binds.
    uint16_t tlutIdx = page->tlutIndex;
    if (tlutIdx < gTlutCount) {
        GX_InitTlutObj(&gTlutObjs[tlutIdx], gTlutData + (size_t) tlutIdx * 256u,
                       GX_TL_RGB5A3, 256);
        GX_LoadTlut(&gTlutObjs[tlutIdx], (uint32_t) slot);
    }
    gTlutSlotOwner[slot] = pageId;
    page->tlutSlot = slot;
    return slot;
}

static bool loadTile(int32_t tileIndex) {
    WiiTexTile* tile = &gTiles[tileIndex];
    if (tile->dataSize == 0) return false;

    uint32_t need = roundUpPow2(tile->dataSize);
    uint32_t n = blocksFor(need);
    if (n > gPoolBlockCount) {
        logError("WiiTextures: tile %d needs %u KB but the pool is only %u KB\n",
                 tileIndex, need / 1024u, gPoolBlockCount * (POOL_BLOCK_SIZE / 1024u));
        return false;
    }

    int32_t start = poolFindRun(n);
    while (start < 0) {
        if (!evictOne(tileIndex)) {
            logError("WiiTextures: pool exhausted loading tile %d (%u KB)\n",
                     tileIndex, need / 1024u);
            return false;
        }
        start = poolFindRun(n);
    }

    uint8_t* dst = gPoolBase + (uint32_t) start * POOL_BLOCK_SIZE;

    if (fseek(gPackFp, (long) tile->dataOffset, SEEK_SET) != 0) return false;
    if (fread(dst, 1, tile->dataSize, gPackFp) != tile->dataSize) return false;

    // GX reads texture memory behind the CPU's back, so this has to leave the data cache.
    DCFlushRange(dst, tile->dataSize);

    bool isCI8 = (tile->flags & WII_TEXFMT_MASK) == WII_TEXFMT_CI8;
    int32_t slot = -1;
    if (isCI8) {
        slot = acquirePageTlut(tile->pageId);
        if (slot < 0) {
            logError("WiiTextures: no TLUT slot for page %d\n", tile->pageId);
            return false;
        }
    }

    poolMark((uint32_t) start, n, tileIndex);
    tile->texels = dst;
    tile->resident = true;
    gPages[tile->pageId].residentTiles++;
    gResidentBytes += need;
    gResidentCount++;
    gStreamIns++;

    if (isCI8) {
        GX_InitTexObjCI(&tile->texObj, dst, tile->width, tile->height,
                        GX_TF_CI8, GX_CLAMP, GX_CLAMP, GX_FALSE, (uint32_t) slot);
    } else {
        GX_InitTexObj(&tile->texObj, dst, tile->width, tile->height,
                      GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
    }
    // Undertale is pixel art drawn at integer scale: bilinear only smears it.
    GX_InitTexObjFilterMode(&tile->texObj, GX_NEAR, GX_NEAR);

    // The GPU caches texels in TMEM by main-memory address, and this pool reuses
    // addresses, so stale lines have to be dropped or the new tile renders as the old
    // one's artwork.
    GX_InvalidateTexAll();

    return true;
}

bool WiiTextures_init(const char* packPath, uint32_t budgetBytes) {
    if (gInitialized) return true;

    gPackFp = fopen(packPath, "rb");
    if (gPackFp == nullptr) {
        logWarn("WiiTextures: cannot open texture pack '%s', running without textures\n", packPath);
        return false;
    }

    uint8_t header[16];
    if (fread(header, 1, sizeof(header), gPackFp) != sizeof(header)) goto fail;
    if (readU32BE(header) != WII_TEX_PACK_MAGIC) {
        logError("WiiTextures: bad pack magic\n");
        goto fail;
    }
    if (readU16BE(header + 4) != WII_TEX_PACK_VERSION) {
        logError("WiiTextures: pack version %u, this build expects %u -- regenerate it\n",
                 readU16BE(header + 4), WII_TEX_PACK_VERSION);
        goto fail;
    }
    gPageCount = (int32_t) readU16BE(header + 6);
    gTileCount = (int32_t) readU16BE(header + 8);
    gTlutCount = readU16BE(header + 10);

    if (gPageCount <= 0 || gTileCount <= 0) goto fail;

    gPages = (WiiTexPage*) calloc((size_t) gPageCount, sizeof(WiiTexPage));
    gTiles = (WiiTexTile*) calloc((size_t) gTileCount, sizeof(WiiTexTile));
    if (gPages == nullptr || gTiles == nullptr) goto fail;

    for (int32_t i = 0; i < gPageCount; i++) {
        uint8_t e[12];
        if (fread(e, 1, sizeof(e), gPackFp) != sizeof(e)) goto fail;
        gPages[i].width     = readU16BE(e + 0);
        gPages[i].height    = readU16BE(e + 2);
        gPages[i].cols      = readU16BE(e + 4);
        gPages[i].rows      = readU16BE(e + 6);
        gPages[i].firstTile = readU16BE(e + 8);
        gPages[i].tlutIndex = readU16BE(e + 10);
        gPages[i].tlutSlot  = -1;
    }

    for (int32_t i = 0; i < gTileCount; i++) {
        uint8_t e[16];
        if (fread(e, 1, sizeof(e), gPackFp) != sizeof(e)) goto fail;
        gTiles[i].width      = readU16BE(e + 0);
        gTiles[i].height     = readU16BE(e + 2);
        gTiles[i].flags      = readU16BE(e + 4);
        gTiles[i].dataOffset = readU32BE(e + 8);
        gTiles[i].dataSize   = readU32BE(e + 12);
        gTiles[i].pageId     = -1;
    }

    // Point each tile back at its page, so eviction can find the shared palette.
    for (int32_t p = 0; p < gPageCount; p++) {
        int32_t n = gPages[p].cols * gPages[p].rows;
        for (int32_t t = 0; t < n; t++) {
            int32_t idx = gPages[p].firstTile + t;
            if (idx >= 0 && idx < gTileCount) gTiles[idx].pageId = p;
        }
    }

    for (int32_t i = 0; i < WII_MAX_TLUT_SLOTS; i++) gTlutSlotOwner[i] = -1;

    if (gTlutCount > 0) {
        size_t tlutBytes = (size_t) gTlutCount * 256u * 2u;
        gTlutData = (uint16_t*) memalign(32, tlutBytes);
        gTlutObjs = (GXTlutObj*) calloc(gTlutCount, sizeof(GXTlutObj));
        if (gTlutData == nullptr || gTlutObjs == nullptr) goto fail;
        if (fread(gTlutData, 1, tlutBytes, gPackFp) != tlutBytes) goto fail;
        DCFlushRange(gTlutData, tlutBytes);
    }

    {
        uint32_t arenaLo = (uint32_t) SYS_GetArena2Lo();
        uint32_t arenaHi = (uint32_t) SYS_GetArena2Hi();
        uint32_t available = (arenaHi > arenaLo) ? (arenaHi - arenaLo) : 0u;

        uint32_t budget = budgetBytes;
        logInfo("WiiTextures: MEM2 free %u KB, asking for %u KB\n",
                available / 1024u, budget / 1024u);
        if (budget > available) {
            logWarn("WiiTextures: asked for %u KB of MEM2 but only %u KB is free\n",
                    budget / 1024u, available / 1024u);
            budget = available;
        }
        if (budget > POOL_BLOCK_SIZE * 4u) budget -= POOL_BLOCK_SIZE * 2u;

        gPoolBlockCount = budget / POOL_BLOCK_SIZE;
        if (gPoolBlockCount > POOL_MAX_BLOCKS) gPoolBlockCount = POOL_MAX_BLOCKS;
        if (gPoolBlockCount == 0) goto fail;

        gPoolBase = (uint8_t*) arenaLo;
        uint32_t misalign = ((uint32_t) gPoolBase) & 31u;
        if (misalign != 0) gPoolBase += (32u - misalign);

        uint32_t poolBytes = gPoolBlockCount * POOL_BLOCK_SIZE;
        SYS_SetArena2Lo((void*) (gPoolBase + poolBytes));
        for (uint32_t i = 0; i < gPoolBlockCount; i++) gBlockOwner[i] = -1;

        logInfo("WiiTextures: %d pages in %d tiles (max %dx%d), pool %u KB in MEM2\n",
                gPageCount, gTileCount, WII_MAX_TEXTURE_DIM, WII_MAX_TEXTURE_DIM,
                poolBytes / 1024u);
    }

    gInitialized = true;
    return true;

fail:
    if (gPackFp != nullptr) { fclose(gPackFp); gPackFp = nullptr; }
    free(gPages);    gPages = nullptr;
    free(gTiles);    gTiles = nullptr;
    free(gTlutObjs); gTlutObjs = nullptr;
    gPageCount = 0;
    gTileCount = 0;
    return false;
}

void WiiTextures_destroy(void) {
    if (!gInitialized) return;
    logInfo("WiiTextures: %u tiles streamed in, %u evictions, %u forced GPU stalls\n",
            gStreamIns, gEvictions, gForcedFlushes);
    for (int32_t i = 0; i < gTileCount; i++) unloadTile(i);
    if (gPackFp != nullptr) { fclose(gPackFp); gPackFp = nullptr; }
    free(gPages);    gPages = nullptr;
    free(gTiles);    gTiles = nullptr;
    free(gTlutObjs); gTlutObjs = nullptr;
    gPageCount = 0;
    gTileCount = 0;
    gInitialized = false;
}

bool WiiTextures_isAvailable(void) { return gInitialized; }

void WiiTextures_beginFrame(uint64_t frameCount) {
    gCurrentFrame = frameCount;

    // Report cache pressure once a second, and only when there is any.
    //
    // Streaming a tile means reading up to 2 MB off the card. A handful of those in a
    // second is normal for a room change; dozens every second means the working set does
    // not fit and the same tiles are being fetched again and again, which reads as the
    // game running slowly when the cause is the disk. Saying which it is costs one line a
    // second and saves guessing at the frame rate.
    static uint64_t windowStart;
    static uint32_t windowStreamIns, windowEvictions;

    if (frameCount < windowStart) windowStart = frameCount;   // a reset restarts the window
    if (frameCount - windowStart >= 30u) {
        uint32_t streamed = gStreamIns - windowStreamIns;
        uint32_t evicted  = gEvictions - windowEvictions;
        if (streamed > 8u || evicted > 0u) {
            logWarn("WiiTextures: %u tiles streamed and %u evicted in the last 30 frames "
                    "(%u KB of %u KB resident)\n",
                    streamed, evicted, gResidentBytes / 1024u,
                    gPoolBlockCount * (POOL_BLOCK_SIZE / 1024u));
        }
        windowStart = frameCount;
        windowStreamIns = gStreamIns;
        windowEvictions = gEvictions;
    }
}

uint16_t WiiTextures_getWidth(int32_t pageId) {
    if (!gInitialized || pageId < 0 || pageId >= gPageCount) return 0;
    return gPages[pageId].width;
}

uint16_t WiiTextures_getHeight(int32_t pageId) {
    if (!gInitialized || pageId < 0 || pageId >= gPageCount) return 0;
    return gPages[pageId].height;
}

bool WiiTextures_getGrid(int32_t pageId, int32_t* outCols, int32_t* outRows) {
    if (!gInitialized || pageId < 0 || pageId >= gPageCount) return false;
    if (gPages[pageId].cols == 0 || gPages[pageId].rows == 0) return false;
    if (outCols) *outCols = gPages[pageId].cols;
    if (outRows) *outRows = gPages[pageId].rows;
    return true;
}

GXTexObj* WiiTextures_getTile(int32_t pageId, int32_t col, int32_t row,
                              int32_t* outX, int32_t* outY, int32_t* outW, int32_t* outH) {
    if (!gInitialized || pageId < 0 || pageId >= gPageCount) return nullptr;
    WiiTexPage* page = &gPages[pageId];
    if (col < 0 || row < 0 || col >= page->cols || row >= page->rows) return nullptr;

    int32_t tileIndex = page->firstTile + row * page->cols + col;
    if (tileIndex < 0 || tileIndex >= gTileCount) return nullptr;

    WiiTexTile* tile = &gTiles[tileIndex];
    if (!tile->resident && !loadTile(tileIndex)) return nullptr;

    tile->lastUsedFrame = gCurrentFrame;
    if (outX) *outX = col * WII_MAX_TEXTURE_DIM;
    if (outY) *outY = row * WII_MAX_TEXTURE_DIM;
    if (outW) *outW = tile->width;
    if (outH) *outH = tile->height;
    return &tile->texObj;
}

int32_t WiiTextures_enumerateResident(int32_t index, int32_t* outPageId,
                                      int32_t* outCol, int32_t* outRow) {
    if (!gInitialized || index < 0) return -1;
    for (int32_t i = 0; i < gTileCount; i++) {
        if (!gTiles[i].resident) continue;
        if (index-- != 0) continue;

        int32_t pid = gTiles[i].pageId;
        if (outPageId) *outPageId = pid;
        if (pid >= 0 && gPages[pid].cols > 0) {
            int32_t local = i - gPages[pid].firstTile;
            if (outCol) *outCol = local % gPages[pid].cols;
            if (outRow) *outRow = local / gPages[pid].cols;
        }
        return i;
    }
    return -1;
}

GXTexObj* WiiTextures_peekResident(int32_t index) {
    int32_t tileIndex = WiiTextures_enumerateResident(index, nullptr, nullptr, nullptr);
    if (tileIndex < 0) return nullptr;
    return &gTiles[tileIndex].texObj;
}

bool WiiTextures_isPagePaletted(int32_t pageId) {
    if (!gInitialized || pageId < 0 || pageId >= gPageCount) return false;
    int32_t first = gPages[pageId].firstTile;
    if (first < 0 || first >= gTileCount) return false;
    return (gTiles[first].flags & WII_TEXFMT_MASK) == WII_TEXFMT_CI8;
}

int32_t  WiiTextures_getPageCount(void)     { return gPageCount; }
uint32_t WiiTextures_getResidentBytes(void) { return gResidentBytes; }
uint32_t WiiTextures_getBudgetBytes(void)   { return gPoolBlockCount * POOL_BLOCK_SIZE; }
int32_t  WiiTextures_getResidentCount(void) { return gResidentCount; }
uint32_t WiiTextures_getEvictionCount(void) { return gEvictions; }
uint32_t WiiTextures_getStreamInCount(void) { return gStreamIns; }
