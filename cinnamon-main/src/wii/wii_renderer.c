#include "wii_renderer.h"
#include "wii_textures.h"

#include "log.h"
#include "runner.h"
#include "data_win.h"
#include "vm.h"
#include "text_utils.h"
#include "utils.h"

#include <stdlib.h>
#include <malloc.h>
#include "string_compat.h"
#include <math.h>

#include <gccore.h>
#include <ogc/gx.h>
#include <ogc/gu.h>

#define DEG_TO_RAD (3.14159265358979323846f / 180.0f)

// Bounded tracing. The log lands on the SD card, so it has to stay small.
#define TRACE_LIMIT 200
static int gTraceCount;
#define TRACE(...) do { if (gTraceCount < TRACE_LIMIT) { gTraceCount++; logInfo(__VA_ARGS__); } } while (0)

// One line per distinct tpag rather than per draw. A single background sprite is drawn
// every frame and would otherwise eat the whole budget before any character is seen --
// which is exactly what happened on the first attempt.
static uint8_t* gSeenTpag;
static uint32_t gSeenTpagCount;

static bool traceOnceForTpag(int32_t tpagIndex) {
    if (gSeenTpag == nullptr || tpagIndex < 0 || (uint32_t) tpagIndex >= gSeenTpagCount) return false;
    if (gSeenTpag[tpagIndex]) return false;
    gSeenTpag[tpagIndex] = 1;
    return true;
}

// MAX_SURFACES comes from runner.h: it is the bound the runner itself uses when it
// hands out surface ids, so the table here has to agree with it rather than pick its own.

typedef struct {
    bool     used;
    int32_t  width;
    int32_t  height;
    void*    texels;      // MEM2, GX-tiled RGBA8
    GXTexObj texObj;
} WiiSurface;

typedef struct {
    Renderer base;

    GXRModeObj* rmode;
    void*       framebuffer[2];
    int         fbIndex;
    void*       fifo;

    int32_t screenW;
    int32_t screenH;

    // ---- pipeline state mirrored from the vtable setters ----
    bool         blendEnable;
    int32_t      blendMode;
    BlendFactors factors;
    bool         alphaTestEnable;
    uint8_t      alphaTestRef;
    bool         colorWrite[4];
    bool         fogEnable;
    uint32_t     fogColor;

    // ---- render targets ----
    WiiSurface surfaces[MAX_SURFACES];
    int32_t    currentTarget;   // RENDER_TARGET_HOST_FRAMEBUFFER or a surface id

    // ---- viewport currently in force, needed when resolving the EFB ----
    int32_t portX, portY, portW, portH;

    // ---- last view drawn, so screen positions can be mapped back to room space ----
    int32_t lastViewX, lastViewY, lastViewW, lastViewH;
    int32_t lastPortX, lastPortY, lastPortW, lastPortH;
    bool    haveLastView;

    uint64_t frameCount;
    bool     dirty;             // something was drawn since the last EFB resolve
} WiiRenderer;

// ===[ Colour helpers ]===
// GameMaker packs colours as BGR, matching the Windows COLORREF the original runner uses.
static inline GXColor toGXColor(uint32_t bgr, float alpha) {
    GXColor c;
    c.r = (uint8_t) ( bgr        & 0xFFu);
    c.g = (uint8_t) ((bgr >>  8) & 0xFFu);
    c.b = (uint8_t) ((bgr >> 16) & 0xFFu);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    c.a = (uint8_t) (alpha * 255.0f + 0.5f);
    return c;
}

// ===[ Blend factor translation ]===
// GameMaker's factors come from Direct3D 9. GX covers most of them, with two gaps that
// are mapped to the nearest legal value rather than silently dropped:
//   * bm_src_alpha_sat has no GX equivalent.
//   * GX only accepts SRCCLR as a destination factor and DSTCLR as a source factor, so
//     the "wrong side" uses is clamped.
static uint8_t gmlFactorToGX(int32_t factor, bool isSource) {
    switch (factor) {
        case bm_zero:            return GX_BL_ZERO;
        case bm_one:             return GX_BL_ONE;
        case bm_src_color:       return isSource ? GX_BL_DSTCLR : GX_BL_SRCCLR;
        case bm_inv_src_color:   return isSource ? GX_BL_INVDSTCLR : GX_BL_INVSRCCLR;
        case bm_src_alpha:       return GX_BL_SRCALPHA;
        case bm_inv_src_alpha:   return GX_BL_INVSRCALPHA;
        case bm_dest_alpha:      return GX_BL_DSTALPHA;
        case bm_inv_dest_alpha:  return GX_BL_INVDSTALPHA;
        case bm_dest_color:      return isSource ? GX_BL_DSTCLR : GX_BL_SRCCLR;
        case bm_inv_dest_color:  return isSource ? GX_BL_INVDSTCLR : GX_BL_INVSRCCLR;
        case bm_src_alpha_sat:   return GX_BL_SRCALPHA;
        default:                 return isSource ? GX_BL_SRCALPHA : GX_BL_INVSRCALPHA;
    }
}

static void applyBlendState(WiiRenderer* r) {
    if (!r->blendEnable) {
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
        return;
    }

    // GX exposes subtract as a blend equation of its own rather than as a factor pair.
    if (r->blendMode == bm_subtract) {
        GX_SetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_COPY);
        return;
    }

    uint8_t src = gmlFactorToGX(r->factors.src, true);
    uint8_t dst = gmlFactorToGX(r->factors.dst, false);
    GX_SetBlendMode(GX_BM_BLEND, src, dst, GX_LO_COPY);
}

static void applyAlphaTest(WiiRenderer* r) {
    if (r->alphaTestEnable) {
        GX_SetAlphaCompare(GX_GREATER, r->alphaTestRef, GX_AOP_AND, GX_ALWAYS, 0);
    } else {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    }
    // Undertale draws strictly back-to-front with no depth buffer, so z is always
    // written as "pass" and the update is left on to keep the EFB coherent.
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_TRUE);
}

// Selects a single TEV stage that either modulates a texture by the vertex colour, or
// passes the vertex colour straight through for untextured geometry.
static void setTextured(bool textured) {
    if (textured) {
        GX_SetNumChans(1);
        GX_SetNumTexGens(1);
        GX_SetNumTevStages(1);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    } else {
        GX_SetNumChans(1);
        GX_SetNumTexGens(0);
        GX_SetNumTevStages(1);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
    }
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
}

// ===[ Primitive helpers ]===

static void quadTextured(float x0, float y0, float x1, float y1,
                         float x2, float y2, float x3, float y3,
                         float u0, float v0, float u1, float v1,
                         GXColor c) {
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(x0, y0); GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(u0, v0);
        GX_Position2f32(x1, y1); GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(u1, v0);
        GX_Position2f32(x2, y2); GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(u1, v1);
        GX_Position2f32(x3, y3); GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(u0, v1);
    GX_End();
}

static void quadFlat(float x0, float y0, float x1, float y1,
                     float x2, float y2, float x3, float y3,
                     GXColor a, GXColor b, GXColor c, GXColor d) {
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(x0, y0); GX_Color4u8(a.r, a.g, a.b, a.a);
        GX_Position2f32(x1, y1); GX_Color4u8(b.r, b.g, b.b, b.a);
        GX_Position2f32(x2, y2); GX_Color4u8(c.r, c.g, c.b, c.a);
        GX_Position2f32(x3, y3); GX_Color4u8(d.r, d.g, d.b, d.a);
    GX_End();
}

// Rotates the four corners of a sprite quad about its origin.
static void buildQuad(float x, float y, float w, float h,
                      float originX, float originY,
                      float xscale, float yscale, float angleDeg,
                      float* outX, float* outY) {
    float left   = -originX * xscale;
    float top    = -originY * yscale;
    float right  = left + w * xscale;
    float bottom = top  + h * yscale;

    float cx[4] = { left, right, right, left  };
    float cy[4] = { top,  top,   bottom, bottom };

    if (angleDeg != 0.0f) {
        // GameMaker angles grow counter-clockwise while screen y grows downward.
        float rad = -angleDeg * DEG_TO_RAD;
        float s = sinf(rad), c = cosf(rad);
        for (int i = 0; i < 4; i++) {
            float px = cx[i], py = cy[i];
            cx[i] = px * c - py * s;
            cy[i] = px * s + py * c;
        }
    }
    for (int i = 0; i < 4; i++) {
        outX[i] = x + cx[i];
        outY[i] = y + cy[i];
    }
}

// Resolves a tpag index to its page texture, binding it and returning the UV rect.
// Returns false when the page is unavailable, in which case the caller should skip
// the draw rather than render garbage.
// ===[ Page-space drawing ]===
//
// Everything textured funnels through here. A page is physically a grid of tiles of at
// most 1024x1024, because GX cannot address anything larger, so a source rectangle may
// span up to four of them. The rect is intersected with each tile it touches and drawn as
// one quad per piece, all sharing the caller's transform, which keeps rotation and
// scaling consistent across the seam.
//
// `localX/localY` is where the rect's top-left corner sits in the caller's untransformed
// space; the rect's size there equals its size in texels.
static void drawPageRect(WiiRenderer* r, int32_t pageId,
                         int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                         const Matrix4f* xf, float localX, float localY,
                         GXColor color) {
    if (srcW <= 0 || srcH <= 0) return;

    int32_t cols = 0, rows = 0;
    if (!WiiTextures_getGrid(pageId, &cols, &rows)) return;

    int32_t firstCol = srcX / WII_MAX_TEXTURE_DIM;
    int32_t lastCol  = (srcX + srcW - 1) / WII_MAX_TEXTURE_DIM;
    int32_t firstRow = srcY / WII_MAX_TEXTURE_DIM;
    int32_t lastRow  = (srcY + srcH - 1) / WII_MAX_TEXTURE_DIM;

    if (firstCol < 0) firstCol = 0;
    if (firstRow < 0) firstRow = 0;
    if (lastCol >= cols) lastCol = cols - 1;
    if (lastRow >= rows) lastRow = rows - 1;

    setTextured(true);

    for (int32_t row = firstRow; row <= lastRow; row++) {
        for (int32_t col = firstCol; col <= lastCol; col++) {
            int32_t tx, ty, tw, th;
            GXTexObj* tex = WiiTextures_getTile(pageId, col, row, &tx, &ty, &tw, &th);
            if (tex == nullptr) continue;

            // Intersect the requested rect with this tile, in page coordinates.
            int32_t ix0 = srcX > tx ? srcX : tx;
            int32_t iy0 = srcY > ty ? srcY : ty;
            int32_t ix1 = (srcX + srcW) < (tx + tw) ? (srcX + srcW) : (tx + tw);
            int32_t iy1 = (srcY + srcH) < (ty + th) ? (srcY + srcH) : (ty + th);
            if (ix1 <= ix0 || iy1 <= iy0) continue;

            GX_LoadTexObj(tex, GX_TEXMAP0);

            float u0 = (float) (ix0 - tx) / (float) tw;
            float v0 = (float) (iy0 - ty) / (float) th;
            float u1 = (float) (ix1 - tx) / (float) tw;
            float v1 = (float) (iy1 - ty) / (float) th;

            float lx0 = localX + (float) (ix0 - srcX);
            float ly0 = localY + (float) (iy0 - srcY);
            float lx1 = lx0 + (float) (ix1 - ix0);
            float ly1 = ly0 + (float) (iy1 - iy0);

            float px0, py0, px1, py1, px2, py2, px3, py3;
            Matrix4f_transformPoint(xf, lx0, ly0, &px0, &py0);
            Matrix4f_transformPoint(xf, lx1, ly0, &px1, &py1);
            Matrix4f_transformPoint(xf, lx1, ly1, &px2, &py2);
            Matrix4f_transformPoint(xf, lx0, ly1, &px3, &py3);

            quadTextured(px0, py0, px1, py1, px2, py2, px3, py3, u0, v0, u1, v1, color);
        }
    }
    r->dirty = true;
}

// Tints a draw by the colour of whichever player is currently executing.
//
// The instance running right now is the one the VM has in scope, which during a draw event
// is the instance being drawn. That is enough to give each player's sprites their own

// The transform a sprite draw applies to its local pixel space. GameMaker angles grow
// counter-clockwise while screen y grows downward, hence the negation.
static void spriteTransform(Matrix4f* xf, float x, float y,
                            float xscale, float yscale, float angleDeg) {
    Matrix4f_setTransform2D(xf, x, y, xscale, yscale, -angleDeg * DEG_TO_RAD);
}

// Resolves a tpag index to its entry, or false when it has no usable page.
static bool resolveTpag(WiiRenderer* r, int32_t tpagIndex, const TexturePageItem** outItem) {
    DataWin* dw = r->base.dataWin;
    if (dw == nullptr || tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) return false;

    const TexturePageItem* item = &dw->tpag.items[tpagIndex];
    if (!item->present || item->texturePageId < 0) return false;
    if (WiiTextures_getWidth(item->texturePageId) == 0) return false;

    *outItem = item;
    return true;
}

// ===[ Vtable ]===

static void wiiInit(Renderer* renderer, DataWin* dataWin) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    r->base.dataWin = dataWin;
    logInfo("WiiRenderer: bound to data.win (%u tpag entries)\n",
            dataWin != nullptr ? dataWin->tpag.count : 0u);

    if (dataWin != nullptr && dataWin->tpag.count > 0) {
        gSeenTpagCount = dataWin->tpag.count;
        gSeenTpag = (uint8_t*) calloc(gSeenTpagCount, 1);
    }
}

static void wiiDestroy(Renderer* renderer) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (r->surfaces[i].used && r->surfaces[i].texels != nullptr)
            free(r->surfaces[i].texels);
    }
    free(r);
}

static void setOrtho(WiiRenderer* r, float left, float right, float bottom, float top) {
    Mtx44 proj;
    guOrtho(proj, top, bottom, left, right, -1.0f, 1.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);

    Mtx mv;
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
}

static void wiiBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH,
                          int32_t windowW, int32_t windowH) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    WiiTextures_beginFrame(r->frameCount);

    GX_SetViewport(0.0f, 0.0f, (float) r->screenW, (float) r->screenH, 0.0f, 1.0f);
    GX_SetScissor(0, 0, r->screenW, r->screenH);
    r->portX = 0; r->portY = 0; r->portW = r->screenW; r->portH = r->screenH;

    setOrtho(r, 0.0f, (float) gameW, (float) gameH, 0.0f);
    applyBlendState(r);
    applyAlphaTest(r);
}

static void wiiEndFrameInit(Renderer* renderer) { (void) renderer; }
static void wiiEndFrameEnd(Renderer* renderer)  { (void) renderer; }

static void wiiBeginView(Renderer* renderer, int32_t viewX, int32_t viewY,
                         int32_t viewW, int32_t viewH,
                         int32_t portX, int32_t portY, int32_t portW, int32_t portH,
                         float viewAngle) {
    WiiRenderer* r = (WiiRenderer*) renderer;

    // (was: beginView trace)
    if (0) TRACE("TRACE beginView view=(%d,%d %dx%d) port=(%d,%d %dx%d) angle=%d target=%d\n",
          (int) viewX, (int) viewY, (int) viewW, (int) viewH,
          (int) portX, (int) portY, (int) portW, (int) portH,
          (int) viewAngle, (int) r->currentTarget);

    GX_SetViewport((float) portX, (float) portY, (float) portW, (float) portH, 0.0f, 1.0f);
    GX_SetScissor((uint32_t) portX, (uint32_t) portY, (uint32_t) portW, (uint32_t) portH);
    r->portX = portX; r->portY = portY; r->portW = portW; r->portH = portH;

    r->lastViewX = viewX; r->lastViewY = viewY;
    r->lastViewW = viewW; r->lastViewH = viewH;
    r->lastPortX = portX; r->lastPortY = portY;
    r->lastPortW = portW; r->lastPortH = portH;
    r->haveLastView = true;

    setOrtho(r, (float) viewX, (float) (viewX + viewW),
                (float) (viewY + viewH), (float) viewY);

    if (viewAngle != 0.0f) {
        // View rotation is folded into the model-view matrix about the view centre.
        Mtx mv, rot;
        guMtxIdentity(mv);
        guMtxRotDeg(rot, 'z', -viewAngle);
        guMtxConcat(rot, mv, mv);
        GX_LoadPosMtxImm(mv, GX_PNMTX0);
    }
}

static void wiiEndView(Renderer* renderer) { (void) renderer; }

static void wiiApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix,
                               const Matrix4f* projectionMatrix) {
    // The runner hands us column-major 4x4s; GX wants row-major 4x3 for the model-view
    // and a 4x4 for the projection.
    Mtx44 proj;
    Mtx mv;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            proj[row][col] = projectionMatrix->m[col * 4 + row];
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 4; col++)
            mv[row][col] = viewMatrix->m[col * 4 + row];

    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
}

static void wiiBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH,
                        int32_t portX, int32_t portY, int32_t portW, int32_t portH,
                        int32_t targetSurfaceId) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (0) TRACE("TRACE beginGUI gui=%dx%d port=(%d,%d %dx%d) target=%d\n",
          (int) guiW, (int) guiH, (int) portX, (int) portY, (int) portW, (int) portH,
          (int) targetSurfaceId);
    GX_SetViewport((float) portX, (float) portY, (float) portW, (float) portH, 0.0f, 1.0f);
    GX_SetScissor((uint32_t) portX, (uint32_t) portY, (uint32_t) portW, (uint32_t) portH);
    r->portX = portX; r->portY = portY; r->portW = portW; r->portH = portH;
    setOrtho(r, 0.0f, (float) guiW, (float) guiH, 0.0f);
}

static void wiiSetGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH,
                                int32_t portW, int32_t portH, bool renderingToUserSurface) {
    setOrtho((WiiRenderer*) renderer, 0.0f, (float) guiW, (float) guiH, 0.0f);
}

static void wiiEndGUI(Renderer* renderer) { (void) renderer; }

static void wiiDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y,
                          float originX, float originY, float xscale, float yscale,
                          float angleDeg, uint32_t color, float alpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    const TexturePageItem* item;
    if (!resolveTpag(r, tpagIndex, &item)) return;

    Matrix4f xf;
    spriteTransform(&xf, x, y, xscale, yscale, angleDeg);

    // targetX/targetY are the sprite's offset inside its bounding box: they shift the
    // artwork but must not shift the origin the rotation happens about.
    drawPageRect(r, item->texturePageId,
                 item->sourceX, item->sourceY, item->sourceWidth, item->sourceHeight,
                 &xf,
                 (float) item->targetX - originX,
                 (float) item->targetY - originY,
                 toGXColor(color, alpha));
}

static void wiiDrawSpritePart(Renderer* renderer, int32_t tpagIndex,
                              int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
                              float x, float y, float xscale, float yscale, float angleDeg,
                              float pivotX, float pivotY, uint32_t color, float alpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    const TexturePageItem* item;
    if (!resolveTpag(r, tpagIndex, &item)) return;

    // Clip the requested sub-rect to what the tpag actually covers on the page.
    if (srcOffX < 0) { srcW += srcOffX; srcOffX = 0; }
    if (srcOffY < 0) { srcH += srcOffY; srcOffY = 0; }
    if (srcOffX + srcW > item->sourceWidth)  srcW = item->sourceWidth  - srcOffX;
    if (srcOffY + srcH > item->sourceHeight) srcH = item->sourceHeight - srcOffY;
    if (srcW <= 0 || srcH <= 0) return;

    Matrix4f xf;
    spriteTransform(&xf, x, y, xscale, yscale, angleDeg);

    drawPageRect(r, item->texturePageId,
                 item->sourceX + srcOffX, item->sourceY + srcOffY, srcW, srcH,
                 &xf, -pivotX, -pivotY, toGXColor(color, alpha));
}

static void wiiDrawSpritePos(Renderer* renderer, int32_t tpagIndex,
                             float x1, float y1, float x2, float y2,
                             float x3, float y3, float x4, float y4, float alpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    const TexturePageItem* item;
    if (!resolveTpag(r, tpagIndex, &item)) return;

    int32_t pageId = item->texturePageId;
    int32_t cols = 0, rows = 0;
    if (!WiiTextures_getGrid(pageId, &cols, &rows)) return;

    const int32_t sx = item->sourceX, sy = item->sourceY;
    const int32_t sw = item->sourceWidth, sh = item->sourceHeight;
    if (sw <= 0 || sh <= 0) return;

    GXColor color = toGXColor(r->base.drawColor, alpha);
    setTextured(true);

    // The four corners are arbitrary, so a piece's corners are found by interpolating
    // them bilinearly at the piece's normalised position within the sprite.
    for (int32_t row = sy / WII_MAX_TEXTURE_DIM; row <= (sy + sh - 1) / WII_MAX_TEXTURE_DIM; row++) {
        for (int32_t col = sx / WII_MAX_TEXTURE_DIM; col <= (sx + sw - 1) / WII_MAX_TEXTURE_DIM; col++) {
            if (col < 0 || row < 0 || col >= cols || row >= rows) continue;

            int32_t tx, ty, tw, th;
            GXTexObj* tex = WiiTextures_getTile(pageId, col, row, &tx, &ty, &tw, &th);
            if (tex == nullptr) continue;

            int32_t ix0 = sx > tx ? sx : tx;
            int32_t iy0 = sy > ty ? sy : ty;
            int32_t ix1 = (sx + sw) < (tx + tw) ? (sx + sw) : (tx + tw);
            int32_t iy1 = (sy + sh) < (ty + th) ? (sy + sh) : (ty + th);
            if (ix1 <= ix0 || iy1 <= iy0) continue;

            GX_LoadTexObj(tex, GX_TEXMAP0);

            float fu0 = (float) (ix0 - sx) / (float) sw, fu1 = (float) (ix1 - sx) / (float) sw;
            float fv0 = (float) (iy0 - sy) / (float) sh, fv1 = (float) (iy1 - sy) / (float) sh;

            #define BILERP_X(u, v) ((1.0f-(u))*(1.0f-(v))*x1 + (u)*(1.0f-(v))*x2 + (u)*(v)*x3 + (1.0f-(u))*(v)*x4)
            #define BILERP_Y(u, v) ((1.0f-(u))*(1.0f-(v))*y1 + (u)*(1.0f-(v))*y2 + (u)*(v)*y3 + (1.0f-(u))*(v)*y4)

            quadTextured(BILERP_X(fu0, fv0), BILERP_Y(fu0, fv0),
                         BILERP_X(fu1, fv0), BILERP_Y(fu1, fv0),
                         BILERP_X(fu1, fv1), BILERP_Y(fu1, fv1),
                         BILERP_X(fu0, fv1), BILERP_Y(fu0, fv1),
                         (float) (ix0 - tx) / (float) tw, (float) (iy0 - ty) / (float) th,
                         (float) (ix1 - tx) / (float) tw, (float) (iy1 - ty) / (float) th,
                         color);

            #undef BILERP_X
            #undef BILERP_Y
        }
    }
    r->dirty = true;
}

static void wiiDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                             uint32_t color, float alpha, bool outline) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    GXColor c = toGXColor(color, alpha);
    setTextured(false);
    if (outline) {
        GX_Begin(GX_LINESTRIP, GX_VTXFMT0, 5);
            GX_Position2f32(x1, y1); GX_Color4u8(c.r, c.g, c.b, c.a);
            GX_Position2f32(x2, y1); GX_Color4u8(c.r, c.g, c.b, c.a);
            GX_Position2f32(x2, y2); GX_Color4u8(c.r, c.g, c.b, c.a);
            GX_Position2f32(x1, y2); GX_Color4u8(c.r, c.g, c.b, c.a);
            GX_Position2f32(x1, y1); GX_Color4u8(c.r, c.g, c.b, c.a);
        GX_End();
    } else {
        quadFlat(x1, y1, x2, y1, x2, y2, x1, y2, c, c, c, c);
    }
    r->dirty = true;
}

static void wiiDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                                  uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                                  float alpha, bool outline) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    GXColor a = toGXColor(c1, alpha), b = toGXColor(c2, alpha);
    GXColor c = toGXColor(c3, alpha), d = toGXColor(c4, alpha);
    setTextured(false);
    if (outline) {
        GX_Begin(GX_LINESTRIP, GX_VTXFMT0, 5);
            GX_Position2f32(x1, y1); GX_Color4u8(a.r, a.g, a.b, a.a);
            GX_Position2f32(x2, y1); GX_Color4u8(b.r, b.g, b.b, b.a);
            GX_Position2f32(x2, y2); GX_Color4u8(c.r, c.g, c.b, c.a);
            GX_Position2f32(x1, y2); GX_Color4u8(d.r, d.g, d.b, d.a);
            GX_Position2f32(x1, y1); GX_Color4u8(a.r, a.g, a.b, a.a);
        GX_End();
    } else {
        quadFlat(x1, y1, x2, y1, x2, y2, x1, y2, a, b, c, d);
    }
    r->dirty = true;
}

static void wiiDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2,
                        float width, uint32_t color, float alpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    GXColor c = toGXColor(color, alpha);
    setTextured(false);

    if (width <= 1.0f) {
        GX_Begin(GX_LINES, GX_VTXFMT0, 2);
            GX_Position2f32(x1, y1); GX_Color4u8(c.r, c.g, c.b, c.a);
            GX_Position2f32(x2, y2); GX_Color4u8(c.r, c.g, c.b, c.a);
        GX_End();
    } else {
        // GX has no line width, so a thick line is drawn as a quad along the segment.
        float dx = x2 - x1, dy = y2 - y1;
        float len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0001f) return;
        float nx = -dy / len * width * 0.5f;
        float ny =  dx / len * width * 0.5f;
        quadFlat(x1 + nx, y1 + ny, x2 + nx, y2 + ny,
                 x2 - nx, y2 - ny, x1 - nx, y1 - ny, c, c, c, c);
    }
    r->dirty = true;
}

static void wiiDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                             float width, uint32_t color1, uint32_t color2, float alpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    GXColor a = toGXColor(color1, alpha), b = toGXColor(color2, alpha);
    setTextured(false);
    if (width <= 1.0f) {
        GX_Begin(GX_LINES, GX_VTXFMT0, 2);
            GX_Position2f32(x1, y1); GX_Color4u8(a.r, a.g, a.b, a.a);
            GX_Position2f32(x2, y2); GX_Color4u8(b.r, b.g, b.b, b.a);
        GX_End();
    } else {
        float dx = x2 - x1, dy = y2 - y1;
        float len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0001f) return;
        float nx = -dy / len * width * 0.5f;
        float ny =  dx / len * width * 0.5f;
        quadFlat(x1 + nx, y1 + ny, x2 + nx, y2 + ny,
                 x2 - nx, y2 - ny, x1 - nx, y1 - ny, a, b, b, a);
    }
    r->dirty = true;
}

static void wiiDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                            float x3, float y3, uint32_t c1, uint32_t c2, uint32_t c3,
                            float alpha, bool outline) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    GXColor a = toGXColor(c1, alpha), b = toGXColor(c2, alpha), c = toGXColor(c3, alpha);
    setTextured(false);
    if (outline) {
        GX_Begin(GX_LINESTRIP, GX_VTXFMT0, 4);
            GX_Position2f32(x1, y1); GX_Color4u8(a.r, a.g, a.b, a.a);
            GX_Position2f32(x2, y2); GX_Color4u8(b.r, b.g, b.b, b.a);
            GX_Position2f32(x3, y3); GX_Color4u8(c.r, c.g, c.b, c.a);
            GX_Position2f32(x1, y1); GX_Color4u8(a.r, a.g, a.b, a.a);
        GX_End();
    } else {
        GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
            GX_Position2f32(x1, y1); GX_Color4u8(a.r, a.g, a.b, a.a);
            GX_Position2f32(x2, y2); GX_Color4u8(b.r, b.g, b.b, b.a);
            GX_Position2f32(x3, y3); GX_Color4u8(c.r, c.g, c.b, c.a);
        GX_End();
    }
    r->dirty = true;
}

// ===[ Text ]===
//
// Every GML draw_text* call reaches the renderer through this, so it has to lay the
// glyphs out itself: the shared code does not decompose text into sprite draws. The
// layout follows the legacy GL renderer step for step -- line breaking, alignment,
// kerning and the per-glyph colour gradient -- so text matches the other backends.

typedef struct {
    Font*             font;
    TexturePageItem*  fontTpag;      // single page for a regular font
    int32_t           fontTpagIndex;
    int16_t           pageId;
    int32_t           texW, texH;
    Sprite*           spriteFontSprite; // set instead, for sprite fonts
} WiiFontState;

static bool resolveFontState(WiiRenderer* r, DataWin* dw, Font* font, WiiFontState* st) {
    memset(st, 0, sizeof(*st));
    st->font = font;
    st->fontTpagIndex = -1;
    st->pageId = -1;

    if (!font->isSpriteFont) {
        if (font->tpagIndex < 0 || (uint32_t) font->tpagIndex >= dw->tpag.count) return false;
        st->fontTpagIndex = font->tpagIndex;
        st->fontTpag = &dw->tpag.items[font->tpagIndex];
        st->pageId = st->fontTpag->texturePageId;
        if (st->pageId < 0) return false;
        st->texW = WiiTextures_getWidth(st->pageId);
        st->texH = WiiTextures_getHeight(st->pageId);
        return st->texW > 0 && st->texH > 0;
    }

    if (font->spriteIndex >= 0 && dw->sprt.count > (uint32_t) font->spriteIndex) {
        st->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
        return true;
    }
    return false;
}

// Maps one glyph to its page, UV rect and local position. Sprite fonts keep each glyph
// in its own tpag, regular fonts share one page.
static bool resolveGlyph(WiiRenderer* r, DataWin* dw, WiiFontState* st, FontGlyph* glyph,
                         float cursorX, float cursorY,
                         int32_t* outPage, int32_t* srcX, int32_t* srcY,
                         int32_t* srcW, int32_t* srcH,
                         float* outX0, float* outY0) {
    Font* font = st->font;

    if (font->isSpriteFont && st->spriteFontSprite != nullptr) {
        Sprite* sprite = st->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (glyphIndex < 0 || glyphIndex >= (int32_t) sprite->textureCount) return false;

        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        if (tpagIdx < 0 || (uint32_t) tpagIdx >= dw->tpag.count) return false;

        TexturePageItem* gt = &dw->tpag.items[tpagIdx];
        if (gt->texturePageId < 0) return false;
        if (WiiTextures_getWidth(gt->texturePageId) == 0) return false;

        *outPage = gt->texturePageId;
        *srcX = gt->sourceX;
        *srcY = gt->sourceY;
        *srcW = gt->sourceWidth;
        *srcH = gt->sourceHeight;
        *outX0 = cursorX + (float) glyph->offset;
        *outY0 = cursorY + (float) (int32_t) gt->targetY - (float) font->spriteOriginYAdjust;
        return true;
    }

    if (st->fontTpag == nullptr) return false;
    *outPage = st->pageId;
    *srcX = st->fontTpag->sourceX + glyph->sourceX;
    *srcY = st->fontTpag->sourceY + glyph->sourceY;
    *srcW = glyph->sourceWidth;
    *srcH = glyph->sourceHeight;
    *outX0 = cursorX + (float) glyph->offset;
    *outY0 = cursorY;
    return true;
}

// ===[ Control prompts ]===
//
// The game names PC keys in its on-screen prompts, and a Wii Remote has none of them.
// Every prompt is rewritten to the button that is actually bound in wii_input.c, so what
// the screen asks for is what the player can press. The mapping has to stay in step with
// the tables there.
//
// Substitution happens at draw time rather than in data.win: the game file is left
// untouched, and a wrong mapping is a rebuild away from being fixed.
typedef struct { const char* from; const char* to; } ControlName;

// Order matters: the matcher takes the first entry that fits, so the composite labels
// have to come before the bare key names they contain.
static const ControlName CONTROL_NAMES[] = {
    // The instruction screen's labels, which the game stores as standalone strings.
    { "[Z or ENTER]", "[2 or +]" },
    { "[X or SHIFT]", "[1 or B]" },
    { "[C or CTRL]",  "[-]"      },
    // Quitting is a press here, not a hold, so the label says what actually works.
    { "[Hold ESC]",   "[HOME]"   },
    // Fullscreen means nothing on a console that is always full screen. The line is
    // repurposed to name the d-pad, which the screen otherwise never mentions.
    { "[F4]",         "[D-Pad]"  },
    { "Fullscreen",   "Move"     },

    // Bare key names, for prompts elsewhere in the game.
    { "[Z]",   "[2]"  },
    { "[X]",   "[1]"  },
    { "[C]",   "[-]"  },
    { "ENTER", "+"    },
    { "Enter", "+"    },
    { "ESC",   "HOME" },
    { "Esc",   "HOME" },
};
#define CONTROL_NAME_COUNT (sizeof(CONTROL_NAMES) / sizeof(CONTROL_NAMES[0]))

// Rewrites key names into button names. Returns `text` unchanged when nothing matches,
// so the common case costs one scan and no copying.
static const char* remapControlNames(const char* text, char* buf, size_t bufSize) {
    bool needed = false;
    for (size_t i = 0; i < CONTROL_NAME_COUNT && !needed; i++) {
        if (strstr(text, CONTROL_NAMES[i].from) != nullptr) needed = true;
    }
    if (!needed) return text;

    size_t out = 0;
    for (size_t in = 0; text[in] != '\0'; ) {
        const ControlName* hit = nullptr;
        size_t fromLen = 0;
        for (size_t i = 0; i < CONTROL_NAME_COUNT; i++) {
            size_t len = strlen(CONTROL_NAMES[i].from);
            if (strncmp(text + in, CONTROL_NAMES[i].from, len) == 0) {
                hit = &CONTROL_NAMES[i];
                fromLen = len;
                break;
            }
        }
        if (hit != nullptr) {
            size_t toLen = strlen(hit->to);
            if (out + toLen >= bufSize) break;
            memcpy(buf + out, hit->to, toLen);
            out += toLen;
            in += fromLen;
        } else {
            if (out + 1 >= bufSize) break;
            buf[out++] = text[in++];
        }
    }
    buf[out] = '\0';
    return buf;
}

static void wiiDrawTextColor(Renderer* renderer, const char* text, float x, float y,
                             float xscale, float yscale, float angleDeg,
                             int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4,
                             float alpha, float lineSeparation) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (dw == nullptr || text == nullptr) return;

    // Long strings skip the rewrite rather than being truncated: prompts are short, so
    // anything this size is prose and has no key names worth swapping.
    char remapBuf[512];
    text = remapControlNames(text, remapBuf, sizeof(remapBuf));

    int32_t fontIndex = renderer->drawFont;
    if (fontIndex < 0 || dw->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];
    WiiFontState st;
    if (!resolveFontState(r, dw, font, &st)) return;

    int32_t textLen = (int32_t) strlen(text);
    if (textLen == 0) return;

    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (lineSeparation < 0.0f)
        ? TextUtils_lineStride(font)
        : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0.0f;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * (3.14159265358979323846f / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y,
                            xscale * font->scaleX, yscale * font->scaleY, angleRad);

    setTextured(true);
    int16_t boundPage = -1;

    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineIdx < lineCount; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0.0f;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        float gradientX = 0.0f;

        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) { ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos); hasCh = true; }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != nullptr) {
                float advance = (float) glyph->shift;
                float leftFrac  = (lineWidth > 0.0f) ? (gradientX / lineWidth) : 0.0f;
                float rightFrac = (lineWidth > 0.0f) ? ((gradientX + advance) / lineWidth) : 1.0f;
                int32_t c1 = Color_lerp(_c1, _c2, leftFrac);
                int32_t c2 = Color_lerp(_c1, _c2, rightFrac);
                int32_t c3 = Color_lerp(_c4, _c3, rightFrac);
                int32_t c4 = Color_lerp(_c4, _c3, leftFrac);

                bool drew = false;
                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    int32_t page, sx, sy, sw, sh;
                    float lx0, ly0;
                    if (resolveGlyph(r, dw, &st, glyph, cursorX, cursorY,
                                     &page, &sx, &sy, &sw, &sh, &lx0, &ly0)) {
                        int32_t col = sx / WII_MAX_TEXTURE_DIM;
                        int32_t row = sy / WII_MAX_TEXTURE_DIM;
                        bool oneTile = (col == (sx + sw - 1) / WII_MAX_TEXTURE_DIM) &&
                                       (row == (sy + sh - 1) / WII_MAX_TEXTURE_DIM);

                        int32_t tx, ty, tw, th;
                        GXTexObj* tex = oneTile
                            ? WiiTextures_getTile(page, col, row, &tx, &ty, &tw, &th)
                            : nullptr;

                        if (tex != nullptr) {
                            // The common path: a glyph sits inside one tile, so it can be
                            // drawn as a single quad and keep its four-corner gradient.
                            if (page != boundPage) { boundPage = page; }
                            GX_LoadTexObj(tex, GX_TEXMAP0);
                            setTextured(true);

                            float u0 = (float) (sx - tx) / (float) tw;
                            float v0 = (float) (sy - ty) / (float) th;
                            float u1 = (float) (sx + sw - tx) / (float) tw;
                            float v1 = (float) (sy + sh - ty) / (float) th;

                            float lx1 = lx0 + (float) sw;
                            float ly1 = ly0 + (float) sh;

                            float px0, py0, px1, py1, px2, py2, px3, py3;
                            Matrix4f_transformPoint(&transform, lx0, ly0, &px0, &py0);
                            Matrix4f_transformPoint(&transform, lx1, ly0, &px1, &py1);
                            Matrix4f_transformPoint(&transform, lx1, ly1, &px2, &py2);
                            Matrix4f_transformPoint(&transform, lx0, ly1, &px3, &py3);

                            GXColor g1 = toGXColor((uint32_t) c1, alpha);
                            GXColor g2 = toGXColor((uint32_t) c2, alpha);
                            GXColor g3 = toGXColor((uint32_t) c3, alpha);
                            GXColor g4 = toGXColor((uint32_t) c4, alpha);

                            GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
                                GX_Position2f32(px0, py0); GX_Color4u8(g1.r, g1.g, g1.b, g1.a); GX_TexCoord2f32(u0, v0);
                                GX_Position2f32(px1, py1); GX_Color4u8(g2.r, g2.g, g2.b, g2.a); GX_TexCoord2f32(u1, v0);
                                GX_Position2f32(px2, py2); GX_Color4u8(g3.r, g3.g, g3.b, g3.a); GX_TexCoord2f32(u1, v1);
                                GX_Position2f32(px3, py3); GX_Color4u8(g4.r, g4.g, g4.b, g4.a); GX_TexCoord2f32(u0, v1);
                            GX_End();
                            drew = true;
                        } else if (!oneTile) {
                            // A glyph packed across a tile seam is rare enough that losing
                            // the within-glyph gradient is not worth extra machinery; the
                            // gradient across the line is preserved because these colours
                            // are recomputed per glyph.
                            drawPageRect(r, page, sx, sy, sw, sh, &transform, lx0, ly0,
                                         toGXColor((uint32_t) c1, alpha));
                            drew = true;
                        }
                    }
                }

                cursorX += (float) glyph->shift;
                gradientX += (float) glyph->shift;
                if (drew && hasNext) {
                    float kern = TextUtils_getKerningOffset(glyph, nextCh);
                    cursorX += kern;
                    gradientX += kern;
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        lineStart = (textLen > lineEnd) ? TextUtils_skipNewline(text, lineEnd, textLen) : lineEnd;
    }

    r->dirty = true;
}

static void wiiDrawText(Renderer* renderer, const char* text, float x, float y,
                        float xscale, float yscale, float angleDeg, float lineSeparation) {
    uint32_t c = renderer->drawColor;
    wiiDrawTextColor(renderer, text, x, y, xscale, yscale, angleDeg,
                     (int32_t) c, (int32_t) c, (int32_t) c, (int32_t) c,
                     renderer->drawAlpha, lineSeparation);
}

static void wiiFlush(Renderer* renderer) {
    (void) renderer;
    GX_Flush();
}

static void wiiClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    // GX_SetCopyClear only takes effect at the next EFB resolve, so a mid-frame clear
    // is drawn as a full-viewport quad with blending off.
    GXColor c = toGXColor(color, alpha);
    GX_SetCopyClear(c, GX_MAX_Z24);

    bool wasBlending = r->blendEnable;
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    setTextured(false);
    quadFlat(0.0f, 0.0f, (float) r->portW, 0.0f,
             (float) r->portW, (float) r->portH, 0.0f, (float) r->portH, c, c, c, c);
    r->blendEnable = wasBlending;
    applyBlendState(r);
    r->dirty = true;
}

// ===[ Blend / raster state ]===

static BlendFactors wiiGpuGetBlendFactors(Renderer* renderer) {
    return ((WiiRenderer*) renderer)->factors;
}

static int32_t wiiGpuGetBlendMode(Renderer* renderer) {
    return ((WiiRenderer*) renderer)->blendMode;
}

static void wiiGpuSetBlendMode(Renderer* renderer, int32_t mode) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    r->blendMode = mode;
    switch (mode) {
        case bm_add:
            r->factors.src = bm_src_alpha; r->factors.dst = bm_one; break;
        case bm_subtract:
            r->factors.src = bm_src_alpha; r->factors.dst = bm_one; break;
        case bm_max:
            // GX has no max blend equation; additive is the closest visual match.
            r->factors.src = bm_src_alpha; r->factors.dst = bm_inv_src_color; break;
        case bm_normal:
        default:
            r->factors.src = bm_src_alpha; r->factors.dst = bm_inv_src_alpha; break;
    }
    r->factors.srcAlpha = r->factors.src;
    r->factors.dstAlpha = r->factors.dst;
    applyBlendState(r);
}

static void wiiGpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor,
                                  int32_t sfactorAlpha, int32_t dfactorAlpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    r->blendMode = bm_complex;
    r->factors.src = sfactor;
    r->factors.dst = dfactor;
    // GX applies one factor pair to both colour and alpha: there is no separate alpha
    // equation. The alpha factors are recorded so the getter round-trips, but only the
    // colour pair reaches the hardware.
    r->factors.srcAlpha = sfactorAlpha;
    r->factors.dstAlpha = dfactorAlpha;
    applyBlendState(r);
}

static void wiiGpuSetBlendEnable(Renderer* renderer, bool enable) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    r->blendEnable = enable;
    applyBlendState(r);
}

static bool wiiGpuGetBlendEnable(Renderer* renderer) {
    return ((WiiRenderer*) renderer)->blendEnable;
}

static void wiiGpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    r->alphaTestEnable = enable;
    applyAlphaTest(r);
}

static void wiiGpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    r->alphaTestRef = ref;
    applyAlphaTest(r);
}

static void wiiGpuSetColorWriteEnable(Renderer* renderer, bool red, bool green,
                                      bool blue, bool alpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    r->colorWrite[0] = red; r->colorWrite[1] = green;
    r->colorWrite[2] = blue; r->colorWrite[3] = alpha;
    // GX masks colour and alpha as two groups, not four channels, so a per-channel RGB
    // mask cannot be honoured exactly; anything less than full RGB disables colour.
    GX_SetColorUpdate((red && green && blue) ? GX_TRUE : GX_FALSE);
    GX_SetAlphaUpdate(alpha ? GX_TRUE : GX_FALSE);
}

static void wiiGpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green,
                                      bool* blue, bool* alpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (red)   *red   = r->colorWrite[0];
    if (green) *green = r->colorWrite[1];
    if (blue)  *blue  = r->colorWrite[2];
    if (alpha) *alpha = r->colorWrite[3];
}

static void wiiGpuSetFog(Renderer* renderer, bool enable, uint32_t color) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    r->fogEnable = enable;
    r->fogColor = color;
    // GameMaker uses fog here as a flat colour replacement rather than depth fog, and
    // that is a TEV rewrite rather than GX_SetFog. Left inert until a game needs it.
}

// ===[ Tiles ]===

static void wiiDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex,
                               float originX, float originY, float x, float y,
                               float xscale, float yscale, bool tileX, bool tileY,
                               float roomW, float roomH, uint32_t color, float alpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    DataWin* dw = r->base.dataWin;
    if (dw == nullptr || tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) return;

    const TexturePageItem* item = &dw->tpag.items[tpagIndex];
    if (!item->present) return;

    float tileW = (float) item->sourceWidth  * xscale;
    float tileH = (float) item->sourceHeight * yscale;
    if (tileW <= 0.0f || tileH <= 0.0f) return;

    float startX = x - originX * xscale;
    float startY = y - originY * yscale;
    if (tileX) startX = startX - ceilf(startX / tileW) * tileW;
    if (tileY) startY = startY - ceilf(startY / tileH) * tileH;

    float endX = tileX ? roomW : startX + tileW;
    float endY = tileY ? roomH : startY + tileH;

    for (float py = startY; py < endY; py += tileH) {
        for (float px = startX; px < endX; px += tileW) {
            wiiDrawSprite(renderer, tpagIndex, px, py, 0.0f, 0.0f,
                          xscale, yscale, 0.0f, color, alpha);
            if (!tileX) break;
        }
        if (!tileY) break;
    }
}

static void wiiDrawTiledPart(Renderer* renderer, int32_t tpagIndex,
                             int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                             float dstX, float dstY, float dstW, float dstH,
                             uint32_t color, float alpha) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0.0f || dstH <= 0.0f) return;
    for (float py = dstY; py < dstY + dstH; py += (float) srcH) {
        for (float px = dstX; px < dstX + dstW; px += (float) srcW) {
            float w = (px + (float) srcW > dstX + dstW) ? (dstX + dstW - px) : (float) srcW;
            float h = (py + (float) srcH > dstY + dstH) ? (dstY + dstH - py) : (float) srcH;
            wiiDrawSpritePart(renderer, tpagIndex, srcX, srcY, (int32_t) w, (int32_t) h,
                              px, py, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, color, alpha);
        }
    }
}

// ===[ Surfaces ]===
//
// The application surface is the EFB itself: it is 640x480, the EFB is 640x528, and
// leaving it in place avoids a full-screen copy every frame. ensureApplicationSurface
// therefore returns APPLICATION_SURFACE_ID, the sentinel the vtable documents for
// exactly this case. User-created surfaces get a real MEM2 texture and are filled with
// GX_CopyTex when they are unbound.

static int32_t wiiCreateSurface(Renderer* renderer, int32_t width, int32_t height) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (width <= 0 || height <= 0) return -1;

    for (int32_t i = 0; i < MAX_SURFACES; i++) {
        if (r->surfaces[i].used) continue;

        uint32_t bytes = (uint32_t) GX_GetTexBufferSize((uint16_t) width, (uint16_t) height,
                                                        GX_TF_RGBA8, GX_FALSE, 0);
        void* texels = memalign(32, bytes);
        if (texels == nullptr) {
            logError("WiiRenderer: out of memory for a %dx%d surface (%u KB)\n",
                     width, height, bytes / 1024u);
            return -1;
        }
        memset(texels, 0, bytes);
        DCFlushRange(texels, bytes);

        r->surfaces[i].used = true;
        r->surfaces[i].width = width;
        r->surfaces[i].height = height;
        r->surfaces[i].texels = texels;
        GX_InitTexObj(&r->surfaces[i].texObj, texels, (uint16_t) width, (uint16_t) height,
                      GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&r->surfaces[i].texObj, GX_NEAR, GX_NEAR);
        return i;
    }
    logError("WiiRenderer: surface table full (%d entries)\n", MAX_SURFACES);
    return -1;
}

static bool wiiSurfaceExists(Renderer* renderer, int32_t surfaceID) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) return true;
    return surfaceID >= 0 && surfaceID < MAX_SURFACES && r->surfaces[surfaceID].used;
}

// Resolves whatever has been drawn into the EFB out to the bound surface's texture.
static void resolveTargetToTexture(WiiRenderer* r, int32_t surfaceID) {
    if (surfaceID < 0 || surfaceID >= MAX_SURFACES || !r->surfaces[surfaceID].used) return;
    WiiSurface* s = &r->surfaces[surfaceID];

    GX_SetTexCopySrc(0, 0, (uint16_t) s->width, (uint16_t) s->height);
    GX_SetTexCopyDst((uint16_t) s->width, (uint16_t) s->height, GX_TF_RGBA8, GX_FALSE);
    GX_CopyTex(s->texels, GX_FALSE);
    GX_PixModeSync();
}

static bool wiiSetRenderTarget(Renderer* renderer, int32_t surfaceID,
                               bool implicitApplicationSurface) {
    WiiRenderer* r = (WiiRenderer*) renderer;

    if (0) TRACE("TRACE setRenderTarget %d -> %d implicit=%d appSurf=%d\n",
          (int) r->currentTarget, (int) surfaceID, implicitApplicationSurface ? 1 : 0,
          r->base.runner != nullptr ? (int) r->base.runner->applicationSurfaceId : -999);

    // Leaving a real surface: fold what was drawn into its texture before switching.
    if (r->currentTarget >= 0 && r->currentTarget != surfaceID) {
        resolveTargetToTexture(r, r->currentTarget);
    }

    r->currentTarget = surfaceID;

    if (surfaceID == RENDER_TARGET_HOST_FRAMEBUFFER || surfaceID == APPLICATION_SURFACE_ID) {
        GX_SetViewport(0.0f, 0.0f, (float) r->screenW, (float) r->screenH, 0.0f, 1.0f);
        GX_SetScissor(0, 0, r->screenW, r->screenH);
        r->portW = r->screenW; r->portH = r->screenH;
        return true;
    }

    if (surfaceID < 0 || surfaceID >= MAX_SURFACES || !r->surfaces[surfaceID].used)
        return false;

    WiiSurface* s = &r->surfaces[surfaceID];
    // A surface larger than the EFB cannot be rendered in one pass. Undertale never
    // asks for one, so this is reported rather than silently producing a cropped image.
    if (s->width > r->rmode->fbWidth || s->height > r->rmode->efbHeight) {
        logWarn("WiiRenderer: surface %d is %dx%d, larger than the EFB (%dx%d)\n",
                surfaceID, s->width, s->height, r->rmode->fbWidth, r->rmode->efbHeight);
    }
    GX_SetViewport(0.0f, 0.0f, (float) s->width, (float) s->height, 0.0f, 1.0f);
    GX_SetScissor(0, 0, (uint32_t) s->width, (uint32_t) s->height);
    r->portW = s->width; r->portH = s->height;
    return true;
}

static int32_t wiiEnsureApplicationSurface(Renderer* renderer, int32_t width, int32_t height) {
    (void) renderer; (void) width; (void) height;
    return APPLICATION_SURFACE_ID;
}

static float wiiGetSurfaceWidth(Renderer* renderer, int32_t surfaceID) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) return (float) r->screenW;
    if (surfaceID < 0 || surfaceID >= MAX_SURFACES || !r->surfaces[surfaceID].used) return 0.0f;
    return (float) r->surfaces[surfaceID].width;
}

static float wiiGetSurfaceHeight(Renderer* renderer, int32_t surfaceID) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) return (float) r->screenH;
    if (surfaceID < 0 || surfaceID >= MAX_SURFACES || !r->surfaces[surfaceID].used) return 0.0f;
    return (float) r->surfaces[surfaceID].height;
}

static void wiiDrawSurface(Renderer* renderer, int32_t surfaceID,
                           int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight,
                           float x, float y, float xscale, float yscale, float angleDeg,
                           uint32_t color, float alpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (surfaceID < 0 || surfaceID >= MAX_SURFACES || !r->surfaces[surfaceID].used) return;
    WiiSurface* s = &r->surfaces[surfaceID];

    if (srcWidth  <= 0) srcWidth  = s->width;
    if (srcHeight <= 0) srcHeight = s->height;

    GX_LoadTexObj(&s->texObj, GX_TEXMAP0);

    float u0 = (float) srcLeft / (float) s->width;
    float v0 = (float) srcTop  / (float) s->height;
    float u1 = (float) (srcLeft + srcWidth)  / (float) s->width;
    float v1 = (float) (srcTop  + srcHeight) / (float) s->height;

    float qx[4], qy[4];
    buildQuad(x, y, (float) srcWidth, (float) srcHeight, 0.0f, 0.0f,
              xscale, yscale, angleDeg, qx, qy);

    setTextured(true);
    quadTextured(qx[0], qy[0], qx[1], qy[1], qx[2], qy[2], qx[3], qy[3],
                 u0, v0, u1, v1, toGXColor(color, alpha));
    r->dirty = true;
}

static void wiiDrawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y,
                                float xscale, float yscale, float roomW, float roomH,
                                uint32_t color, float alpha) {
    // Documented in the vtable as modern-GL only; the console renderers stub it.
    (void) renderer; (void) surfaceID; (void) x; (void) y;
    (void) xscale; (void) yscale; (void) roomW; (void) roomH; (void) color; (void) alpha;
}

static void wiiSurfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (surfaceID < 0 || surfaceID >= MAX_SURFACES || !r->surfaces[surfaceID].used) return;
    WiiSurface* s = &r->surfaces[surfaceID];
    if (s->width == width && s->height == height) return;

    uint32_t bytes = (uint32_t) GX_GetTexBufferSize((uint16_t) width, (uint16_t) height,
                                                    GX_TF_RGBA8, GX_FALSE, 0);
    void* texels = memalign(32, bytes);
    if (texels == nullptr) {
        logError("WiiRenderer: out of memory resizing surface %d to %dx%d\n",
                 surfaceID, width, height);
        return;
    }
    memset(texels, 0, bytes);
    DCFlushRange(texels, bytes);
    free(s->texels);

    s->texels = texels;
    s->width = width;
    s->height = height;
    GX_InitTexObj(&s->texObj, texels, (uint16_t) width, (uint16_t) height,
                  GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&s->texObj, GX_NEAR, GX_NEAR);
}

static void wiiSurfaceFree(Renderer* renderer, int32_t surfaceID) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (surfaceID < 0 || surfaceID >= MAX_SURFACES || !r->surfaces[surfaceID].used) return;
    free(r->surfaces[surfaceID].texels);
    r->surfaces[surfaceID].texels = nullptr;
    r->surfaces[surfaceID].used = false;
    if (r->currentTarget == surfaceID) r->currentTarget = RENDER_TARGET_HOST_FRAMEBUFFER;
}

static void wiiSurfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY,
                           int32_t srcSurfaceID, int32_t srcX, int32_t srcY,
                           int32_t srcW, int32_t srcH, bool part) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (!wiiSurfaceExists(renderer, srcSurfaceID)) return;

    int32_t previous = r->currentTarget;
    if (!wiiSetRenderTarget(renderer, destSurfaceID, false)) return;

    if (!part) {
        srcX = 0; srcY = 0;
        srcW = (int32_t) wiiGetSurfaceWidth(renderer, srcSurfaceID);
        srcH = (int32_t) wiiGetSurfaceHeight(renderer, srcSurfaceID);
    }
    wiiDrawSurface(renderer, srcSurfaceID, srcX, srcY, srcW, srcH,
                   (float) destX, (float) destY, 1.0f, 1.0f, 0.0f, 0xFFFFFF, 1.0f);

    wiiSetRenderTarget(renderer, previous, false);
}

static bool wiiSurfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA) {
    // Reading a surface back means resolving the EFB and then un-tiling RGBA8 on the
    // CPU. Nothing in Undertale 1.08 calls surface_getpixel, so this reports failure
    // instead of shipping an untested slow path.
    (void) renderer; (void) surfaceID; (void) outRGBA;
    return false;
}

// ===[ Shaders ]===
// The Hollywood has no programmable shaders, only the TEV. Every entry point here
// reports "unsupported" so GML that guards on shaders_are_supported() takes the right
// branch instead of drawing with a silently wrong pipeline.

static void wiiGpuSetShader(Renderer* renderer, int32_t shaderIndex) { (void) renderer; (void) shaderIndex; }
static void wiiGpuResetShader(Renderer* renderer) { (void) renderer; }
static int32_t wiiShaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform) {
    (void) renderer; (void) shaderIndex; (void) uniform; return -1;
}
static int32_t wiiShaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform) {
    (void) renderer; (void) shaderIndex; (void) uniform; return -1;
}
static void wiiShaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count,
                                 float v1, float v2, float v3, float v4) {
    (void) renderer; (void) handle; (void) count; (void) v1; (void) v2; (void) v3; (void) v4;
}
static void wiiShaderSetUniformFArray(Renderer* renderer, int32_t handle, float* values, uint32_t count) {
    (void) renderer; (void) handle; (void) values; (void) count;
}
static void wiiShaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count,
                                 int32_t v1, int32_t v2, int32_t v3, int32_t v4) {
    (void) renderer; (void) handle; (void) count; (void) v1; (void) v2; (void) v3; (void) v4;
}
static bool wiiShaderIsCompiled(Renderer* renderer, int32_t shader) { (void) renderer; (void) shader; return false; }
static bool wiiShadersSupported(void) { return false; }

// ===[ Texture handles ]===
// Handles are 1-based tpag indices so that 0 keeps its "no texture" meaning.

static uint32_t wiiSpriteGetTexture(Renderer* renderer, int32_t tpagIndex) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    DataWin* dw = r->base.dataWin;
    if (dw == nullptr || tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) return 0;
    return (uint32_t) (tpagIndex + 1);
}

static uint32_t wiiSurfaceGetTexture(Renderer* renderer, int32_t surfaceID) {
    if (!wiiSurfaceExists(renderer, surfaceID)) return 0;
    // Surface handles live above the tpag range so the two never collide.
    return 0x40000000u | (uint32_t) surfaceID;
}

static float wiiTextureGetTexelWidth(Renderer* renderer, uint32_t texID) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    DataWin* dw = r->base.dataWin;
    if (texID == 0 || dw == nullptr || (texID & 0x40000000u)) return 0.0f;
    int32_t idx = (int32_t) texID - 1;
    if ((uint32_t) idx >= dw->tpag.count) return 0.0f;
    uint16_t pw = WiiTextures_getWidth(dw->tpag.items[idx].texturePageId);
    return pw != 0 ? 1.0f / (float) pw : 0.0f;
}

static float wiiTextureGetTexelHeight(Renderer* renderer, uint32_t texID) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    DataWin* dw = r->base.dataWin;
    if (texID == 0 || dw == nullptr || (texID & 0x40000000u)) return 0.0f;
    int32_t idx = (int32_t) texID - 1;
    if ((uint32_t) idx >= dw->tpag.count) return 0.0f;
    uint16_t ph = WiiTextures_getHeight(dw->tpag.items[idx].texturePageId);
    return ph != 0 ? 1.0f / (float) ph : 0.0f;
}

static bool wiiTextureGetUVs(Renderer* renderer, uint32_t texID, float* outUVs) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    DataWin* dw = r->base.dataWin;
    if (texID == 0 || outUVs == nullptr) return false;

    if (texID & 0x40000000u) {
        outUVs[0] = 0.0f; outUVs[1] = 0.0f; outUVs[2] = 1.0f; outUVs[3] = 1.0f;
        return true;
    }
    if (dw == nullptr) return false;
    int32_t idx = (int32_t) texID - 1;
    if ((uint32_t) idx >= dw->tpag.count) return false;

    const TexturePageItem* item = &dw->tpag.items[idx];
    uint16_t pw = WiiTextures_getWidth(item->texturePageId);
    uint16_t ph = WiiTextures_getHeight(item->texturePageId);
    if (pw == 0 || ph == 0) return false;

    outUVs[0] = (float) item->sourceX / (float) pw;
    outUVs[1] = (float) item->sourceY / (float) ph;
    outUVs[2] = (float) (item->sourceX + item->sourceWidth)  / (float) pw;
    outUVs[3] = (float) (item->sourceY + item->sourceHeight) / (float) ph;
    return true;
}

static void wiiTextureSetStage(Renderer* renderer, int32_t slot, uint32_t texID) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (slot < 0 || slot >= 8 || texID == 0) return;

    if (texID & 0x40000000u) {
        int32_t sid = (int32_t) (texID & 0x0FFFFFFFu);
        if (sid >= 0 && sid < MAX_SURFACES && r->surfaces[sid].used)
            GX_LoadTexObj(&r->surfaces[sid].texObj, (uint8_t) slot);
        return;
    }
    DataWin* dw = r->base.dataWin;
    if (dw == nullptr) return;
    int32_t idx = (int32_t) texID - 1;
    if ((uint32_t) idx >= dw->tpag.count) return;

    // Binding a whole page to a stage only makes sense for the tile the sprite starts in;
    // a page is physically several textures now, and GX binds one at a time. Shader-style
    // multi-texturing is unsupported on this backend anyway, so the first tile is enough.
    const TexturePageItem* item = &dw->tpag.items[idx];
    int32_t tx, ty, tw, th;
    GXTexObj* tex = WiiTextures_getTile(item->texturePageId,
                                        item->sourceX / WII_MAX_TEXTURE_DIM,
                                        item->sourceY / WII_MAX_TEXTURE_DIM,
                                        &tx, &ty, &tw, &th);
    if (tex != nullptr) GX_LoadTexObj(tex, (uint8_t) slot);
}

static void wiiSetMatrix(Renderer* renderer, int32_t matrixType, Matrix4f matrix) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (matrixType < 0 || matrixType >= MATRICES_MAX) return;
    r->base.gmlMatrices[matrixType] = matrix;

    if (matrixType == MATRIX_WORLD || matrixType == MATRIX_VIEW) {
        Mtx mv;
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 4; col++)
                mv[row][col] = matrix.m[col * 4 + row];
        GX_LoadPosMtxImm(mv, GX_PNMTX0);
    }
}

static int32_t wiiCreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID,
                                          int32_t x, int32_t y, int32_t w, int32_t h,
                                          bool removeback, bool smooth,
                                          int32_t xorig, int32_t yorig) {
    // Needs a CPU readback of the surface, which wiiSurfaceGetPixels does not provide.
    (void) renderer; (void) surfaceID; (void) x; (void) y; (void) w; (void) h;
    (void) removeback; (void) smooth; (void) xorig; (void) yorig;
    return -1;
}

static void wiiDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    (void) renderer; (void) spriteIndex;
}

// No wiiDrawTile: Renderer_drawTile calls the vtable entry only when it is non-null and
// otherwise runs a correct default that clips the tile against the tpag's content rect.
// A stub here silently discards every tile in the room, which is what it did before.

static RendererVtable gWiiVtable = {
    .init = wiiInit,
    .destroy = wiiDestroy,
    .beginFrame = wiiBeginFrame,
    .endFrameInit = wiiEndFrameInit,
    .endFrameEnd = wiiEndFrameEnd,
    .beginView = wiiBeginView,
    .endView = wiiEndView,
    .applyProjection = wiiApplyProjection,
    .beginGUI = wiiBeginGUI,
    .setGuiProjection = wiiSetGuiProjection,
    .endGUI = wiiEndGUI,
    .drawSprite = wiiDrawSprite,
    .drawSpritePart = wiiDrawSpritePart,
    .drawSpritePos = wiiDrawSpritePos,
    .drawRectangle = wiiDrawRectangle,
    .drawRectangleColor = wiiDrawRectangleColor,
    .drawLine = wiiDrawLine,
    .drawTriangle = wiiDrawTriangle,
    .drawLineColor = wiiDrawLineColor,
    .drawText = wiiDrawText,
    .drawTextColor = wiiDrawTextColor,
    .flush = wiiFlush,
    .clearScreen = wiiClearScreen,
    .createSpriteFromSurface = wiiCreateSpriteFromSurface,
    .deleteSprite = wiiDeleteSprite,
    .gpuGetBlendFactors = wiiGpuGetBlendFactors,
    .gpuGetBlendMode = wiiGpuGetBlendMode,
    .gpuSetBlendMode = wiiGpuSetBlendMode,
    .gpuSetBlendModeExt = wiiGpuSetBlendModeExt,
    .gpuSetBlendEnable = wiiGpuSetBlendEnable,
    .gpuSetAlphaTestEnable = wiiGpuSetAlphaTestEnable,
    .gpuSetAlphaTestRef = wiiGpuSetAlphaTestRef,
    .gpuSetColorWriteEnable = wiiGpuSetColorWriteEnable,
    .gpuGetColorWriteEnable = wiiGpuGetColorWriteEnable,
    .gpuGetBlendEnable = wiiGpuGetBlendEnable,
    .gpuSetFog = wiiGpuSetFog,
    // Left null on purpose: the shared default path handles tiles correctly.
    .drawTile = nullptr,
    .drawSpriteTiled = wiiDrawSpriteTiled,
    .createSurface = wiiCreateSurface,
    .surfaceExists = wiiSurfaceExists,
    .setRenderTarget = wiiSetRenderTarget,
    .ensureApplicationSurface = wiiEnsureApplicationSurface,
    .getSurfaceWidth = wiiGetSurfaceWidth,
    .getSurfaceHeight = wiiGetSurfaceHeight,
    .drawSurface = wiiDrawSurface,
    .drawSurfaceTiled = wiiDrawSurfaceTiled,
    .surfaceResize = wiiSurfaceResize,
    .surfaceFree = wiiSurfaceFree,
    .surfaceCopy = wiiSurfaceCopy,
    .surfaceGetPixels = wiiSurfaceGetPixels,
    .drawTiledPart = wiiDrawTiledPart,
    .gpuSetShader = wiiGpuSetShader,
    .gpuResetShader = wiiGpuResetShader,
    .shaderGetUniform = wiiShaderGetUniform,
    .shaderGetSamplerIndex = wiiShaderGetSamplerIndex,
    .shaderSetUniformF = wiiShaderSetUniformF,
    .shaderSetUniformFArray = wiiShaderSetUniformFArray,
    .shaderSetUniformI = wiiShaderSetUniformI,
    .spriteGetTexture = wiiSpriteGetTexture,
    .surfaceGetTexture = wiiSurfaceGetTexture,
    .textureGetTexelWidth = wiiTextureGetTexelWidth,
    .textureGetTexelHeight = wiiTextureGetTexelHeight,
    .textureGetUVs = wiiTextureGetUVs,
    .textureSetStage = wiiTextureSetStage,
    .shaderIsCompiled = wiiShaderIsCompiled,
    .shadersSupported = wiiShadersSupported,
    .setMatrix = wiiSetMatrix,
};

Renderer* WiiRenderer_create(int screenWidth, int screenHeight) {
    WiiRenderer* r = (WiiRenderer*) calloc(1, sizeof(WiiRenderer));
    if (r == nullptr) {
        logError("WiiRenderer: out of memory\n");
        return nullptr;
    }

    r->base.vtable = &gWiiVtable;
    r->base.drawColor = 0xFFFFFF;
    r->base.drawAlpha = 1.0f;
    r->base.drawFont = -1;
    r->base.circlePrecision = 24;
    r->base.currentShader = -1;

    r->rmode = VIDEO_GetPreferredMode(nullptr);
    r->screenW = screenWidth  > 0 ? screenWidth  : r->rmode->fbWidth;
    r->screenH = screenHeight > 0 ? screenHeight : r->rmode->efbHeight;

    r->blendEnable = true;
    r->blendMode = bm_normal;
    r->factors.src = bm_src_alpha;
    r->factors.dst = bm_inv_src_alpha;
    r->factors.srcAlpha = bm_src_alpha;
    r->factors.dstAlpha = bm_inv_src_alpha;
    for (int i = 0; i < 4; i++) r->colorWrite[i] = true;
    r->currentTarget = RENDER_TARGET_HOST_FRAMEBUFFER;

    // The vertex format never changes: 2D position, one RGBA8 colour, one UV pair.
    GX_ClearVtxDesc();
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XY,  GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,  GX_F32,   0);

    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    applyBlendState(r);
    applyAlphaTest(r);

    logInfo("WiiRenderer: GX renderer up at %dx%d\n", r->screenW, r->screenH);
    return (Renderer*) r;
}

void WiiRenderer_setFrameCount(Renderer* renderer, unsigned long long frame) {
    ((WiiRenderer*) renderer)->frameCount = (uint64_t) frame;
}

bool WiiRenderer_getLastView(Renderer* renderer,
                             int32_t* viewX, int32_t* viewY, int32_t* viewW, int32_t* viewH,
                             int32_t* portX, int32_t* portY, int32_t* portW, int32_t* portH) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (!r->haveLastView || r->lastPortW <= 0 || r->lastPortH <= 0) return false;
    if (viewX) *viewX = r->lastViewX;
    if (viewY) *viewY = r->lastViewY;
    if (viewW) *viewW = r->lastViewW;
    if (viewH) *viewH = r->lastViewH;
    if (portX) *portX = r->lastPortX;
    if (portY) *portY = r->lastPortY;
    if (portW) *portW = r->lastPortW;
    if (portH) *portH = r->lastPortH;
    return true;
}

int32_t WiiRenderer_uiFontIndex(Renderer* renderer) {
    static int32_t cached = -2; // -2 = not looked up yet
    if (cached != -2) return cached;

    DataWin* dw = renderer->dataWin;
    cached = -1;
    if (dw == nullptr) return cached;

    static const char* PREFERRED[] = {
        "fnt_small",     // Crypt of Tomorrow: compact, fits a dense grid
        "fnt_maintext",
        "fnt_main",
        "fnt_plain",
    };
    for (size_t p = 0; p < sizeof(PREFERRED) / sizeof(PREFERRED[0]); p++) {
        for (uint32_t i = 0; i < dw->font.count; i++) {
            const char* name = dw->font.fonts[i].name;
            if (name == nullptr || dw->font.fonts[i].glyphCount == 0) continue;
            if (strcmp(name, PREFERRED[p]) == 0) {
                cached = (int32_t) i;
                logInfo("WiiRenderer: overlay font %u (%s)\n", i, name);
                return cached;
            }
        }
    }
    for (uint32_t i = 0; i < dw->font.count; i++) {
        const char* name = dw->font.fonts[i].name;
        if (dw->font.fonts[i].glyphCount == 0) continue;
        if (name != nullptr && strstr(name, "wingding") != nullptr) continue;
        cached = (int32_t) i;
        logInfo("WiiRenderer: overlay font falls back to %u (%s)\n",
                i, name != nullptr ? name : "?");
        return cached;
    }
    logWarn("WiiRenderer: no usable overlay font\n");
    return cached;
}

void WiiRenderer_drawRawTexture(Renderer* renderer, GXTexObj* tex,
                                int32_t texW, int32_t texH,
                                int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                                float dstX, float dstY, float dstW, float dstH,
                                uint32_t bgrColor, float alpha) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    if (tex == nullptr || srcW <= 0 || srcH <= 0 || texW <= 0 || texH <= 0) return;
    if (dstW <= 0.0f || dstH <= 0.0f) return;

    setTextured(true);
    GX_LoadTexObj(tex, GX_TEXMAP0);

    float u0 = (float) srcX / (float) texW;
    float v0 = (float) srcY / (float) texH;
    float u1 = (float) (srcX + srcW) / (float) texW;
    float v1 = (float) (srcY + srcH) / (float) texH;

    // No tint is applied here on purpose: the whole point of drawing Kris is that the
    // extra player stops being a recoloured copy of player one.
    GXColor c = toGXColor(bgrColor, alpha);
    quadTextured(dstX, dstY, dstX + dstW, dstY,
                 dstX + dstW, dstY + dstH, dstX, dstY + dstH,
                 u0, v0, u1, v1, c);
    r->dirty = true;
}

void WiiRenderer_overlayText(Renderer* renderer, const char* text,
                             float x, float y, uint32_t bgrColor, float alpha, float scale) {
    int32_t font = WiiRenderer_uiFontIndex(renderer);
    if (font < 0 || text == nullptr) return;

    int32_t savedFont = renderer->drawFont;
    uint32_t savedColor = renderer->drawColor;
    float savedAlpha = renderer->drawAlpha;
    int32_t savedH = renderer->drawHalign, savedV = renderer->drawValign;

    renderer->drawFont = font;
    renderer->drawColor = bgrColor;
    renderer->drawAlpha = alpha;
    renderer->drawHalign = 0;
    renderer->drawValign = 0;
    renderer->vtable->drawText(renderer, text, x, y, scale, scale, 0.0f, -1.0f);

    renderer->drawFont = savedFont;
    renderer->drawColor = savedColor;
    renderer->drawAlpha = savedAlpha;
    renderer->drawHalign = savedH;
    renderer->drawValign = savedV;
}

void WiiRenderer_beginOverlay(Renderer* renderer, int32_t gameW, int32_t gameH) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    GX_SetViewport(0.0f, 0.0f, (float) r->screenW, (float) r->screenH, 0.0f, 1.0f);
    GX_SetScissor(0, 0, (uint32_t) r->screenW, (uint32_t) r->screenH);
    r->portX = 0; r->portY = 0; r->portW = gameW; r->portH = gameH;
    setOrtho(r, 0.0f, (float) gameW, (float) gameH, 0.0f);

    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
}

void WiiRenderer_endOverlay(Renderer* renderer) {
    WiiRenderer* r = (WiiRenderer*) renderer;
    applyBlendState(r);
    applyAlphaTest(r);
}

void WiiRenderer_drawPointer(Renderer* renderer, float x, float y,
                             int32_t gameW, int32_t gameH) {
    WiiRenderer* r = (WiiRenderer*) renderer;

    GX_SetViewport(0.0f, 0.0f, (float) r->screenW, (float) r->screenH, 0.0f, 1.0f);
    GX_SetScissor(0, 0, (uint32_t) r->screenW, (uint32_t) r->screenH);
    setOrtho(r, 0.0f, (float) gameW, (float) gameH, 0.0f);

    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    setTextured(false);

    // Four ticks around a gap rather than a filled dot, so the cursor never hides what is
    // being aimed at. Each tick is drawn twice: a black slab first, then a white one
    // inset inside it. Undertale's rooms are mostly very dark or very bright, and a
    // single-colour cursor disappears into one or the other -- an outlined one cannot.
    GXColor outline = toGXColor(0x000000, 0.85f);
    GXColor core    = toGXColor(0xFFFFFF, 1.0f);

    const float arm = 11.0f, gap = 4.0f, t = 2.0f, ol = 1.5f;

    struct { float x0, y0, x1, y1; } ticks[] = {
        { x - gap - arm, y - t, x - gap,       y + t },
        { x + gap,       y - t, x + gap + arm, y + t },
        { x - t, y - gap - arm, x + t,         y - gap },
        { x - t, y + gap,       x + t,         y + gap + arm },
    };

    for (int pass = 0; pass < 2; pass++) {
        GXColor c = (pass == 0) ? outline : core;
        float grow = (pass == 0) ? ol : 0.0f;
        for (size_t i = 0; i < sizeof(ticks) / sizeof(ticks[0]); i++) {
            float x0 = ticks[i].x0 - grow, y0 = ticks[i].y0 - grow;
            float x1 = ticks[i].x1 + grow, y1 = ticks[i].y1 + grow;
            quadFlat(x0, y0, x1, y0, x1, y1, x0, y1, c, c, c, c);
        }
        // A dot in the middle of the gap marks the exact aim point.
        float d = (pass == 0) ? 2.0f + ol : 2.0f;
        quadFlat(x - d, y - d, x + d, y - d, x + d, y + d, x - d, y + d, c, c, c, c);
    }

    applyBlendState(r);
    applyAlphaTest(r);
    r->dirty = true;
}

void WiiRenderer_drawPageOverlay(Renderer* renderer, int32_t gameW, int32_t gameH) {
    WiiRenderer* r = (WiiRenderer*) renderer;

    GX_SetViewport(0.0f, 0.0f, (float) r->screenW, (float) r->screenH, 0.0f, 1.0f);
    GX_SetScissor(0, 0, (uint32_t) r->screenW, (uint32_t) r->screenH);
    setOrtho(r, 0.0f, (float) gameW, (float) gameH, 0.0f);

    // Opaque, unblended, unfiltered: what is drawn here must be the texels themselves,
    // not the result of the blend state the game happened to leave behind.
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);

    const float w = (float) gameW, h = (float) gameH;
    GXColor black = toGXColor(0x000000, 1.0f);
    setTextured(false);
    quadFlat(0, 0, w, 0, w, h, 0, h, black, black, black, black);

    const int cols = 3, rows = 3;
    const float pad = 6.0f;
    const float cellW = w / (float) cols;
    const float cellH = h / (float) rows;

    for (int i = 0; i < cols * rows; i++) {
        int32_t pageId = -1, tileCol = 0, tileRow = 0;
        if (WiiTextures_enumerateResident(i, &pageId, &tileCol, &tileRow) < 0) break;

        GXTexObj* tex = WiiTextures_peekResident(i);
        if (tex == nullptr) continue;

        float cx = (float) (i % cols) * cellW + pad;
        float cy = (float) (i / cols) * cellH + pad;
        float cw = cellW - pad * 2.0f;
        float ch = cellH - pad * 2.0f - 10.0f; // room for the id readout

        // The page, whole, stretched into the cell.
        GX_LoadTexObj(tex, GX_TEXMAP0);
        setTextured(true);
        GXColor white = toGXColor(0xFFFFFF, 1.0f);
        quadTextured(cx, cy, cx + cw, cy, cx + cw, cy + ch, cx, cy + ch,
                     0.0f, 0.0f, 1.0f, 1.0f, white);

        // Frame: green when palettised, magenta when raw.
        GXColor frame = toGXColor(WiiTextures_isPagePaletted(pageId) ? 0x00FF00 : 0xFF00FF, 1.0f);
        setTextured(false);
        GX_Begin(GX_LINESTRIP, GX_VTXFMT0, 5);
            GX_Position2f32(cx, cy);           GX_Color4u8(frame.r, frame.g, frame.b, 255);
            GX_Position2f32(cx + cw, cy);      GX_Color4u8(frame.r, frame.g, frame.b, 255);
            GX_Position2f32(cx + cw, cy + ch); GX_Color4u8(frame.r, frame.g, frame.b, 255);
            GX_Position2f32(cx, cy + ch);      GX_Color4u8(frame.r, frame.g, frame.b, 255);
            GX_Position2f32(cx, cy);           GX_Color4u8(frame.r, frame.g, frame.b, 255);
        GX_End();

        // Page id in binary, most significant bit first. Five bits covers 0..31, and
        // there are 26 pages.
        for (int bit = 4; bit >= 0; bit--) {
            bool on = ((pageId >> bit) & 1) != 0;
            GXColor c = toGXColor(on ? 0xFFFFFF : 0x303030, 1.0f);
            float bx = cx + (float) (4 - bit) * 10.0f;
            float by = cy + ch + 2.0f;
            quadFlat(bx, by, bx + 8.0f, by, bx + 8.0f, by + 6.0f, bx, by + 6.0f, c, c, c, c);
        }
    }

    applyBlendState(r);
    applyAlphaTest(r);
}

void WiiRenderer_drawCalibration(Renderer* renderer, int32_t gameW, int32_t gameH) {
    WiiRenderer* r = (WiiRenderer*) renderer;

    // Bind the whole screen and a known projection, so what this draws depends on
    // nothing the runner did this frame.
    GX_SetViewport(0.0f, 0.0f, (float) r->screenW, (float) r->screenH, 0.0f, 1.0f);
    GX_SetScissor(0, 0, (uint32_t) r->screenW, (uint32_t) r->screenH);
    setOrtho(r, 0.0f, (float) gameW, (float) gameH, 0.0f);

    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    setTextured(false);

    const float w = (float) gameW, h = (float) gameH, s = 24.0f;
    struct { float x, y; uint32_t bgr; } marks[] = {
        { 0.0f,     0.0f,     0x0000FF }, // red    top-left
        { w - s,    0.0f,     0x00FF00 }, // green  top-right
        { 0.0f,     h - s,    0xFF0000 }, // blue   bottom-left
        { w - s,    h - s,    0x00FFFF }, // yellow bottom-right
        { w/2 - 8,  h/2 - 8,  0xFFFFFF }, // white  centre
    };

    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
        float side = (i == 4) ? 16.0f : s;
        GXColor c = toGXColor(marks[i].bgr, 1.0f);
        quadFlat(marks[i].x, marks[i].y,
                 marks[i].x + side, marks[i].y,
                 marks[i].x + side, marks[i].y + side,
                 marks[i].x, marks[i].y + side, c, c, c, c);
    }

    // A one-pixel frame on the exact game rectangle: if the TV crops it, the edges are
    // off-screen and the overscan is measurable from how much is missing.
    GXColor line = toGXColor(0xFFFFFF, 1.0f);
    GX_Begin(GX_LINESTRIP, GX_VTXFMT0, 5);
        GX_Position2f32(0.5f,     0.5f);     GX_Color4u8(line.r, line.g, line.b, line.a);
        GX_Position2f32(w - 0.5f, 0.5f);     GX_Color4u8(line.r, line.g, line.b, line.a);
        GX_Position2f32(w - 0.5f, h - 0.5f); GX_Color4u8(line.r, line.g, line.b, line.a);
        GX_Position2f32(0.5f,     h - 0.5f); GX_Color4u8(line.r, line.g, line.b, line.a);
        GX_Position2f32(0.5f,     0.5f);     GX_Color4u8(line.r, line.g, line.b, line.a);
    GX_End();

    applyBlendState(r);
    applyAlphaTest(r);
}

void WiiRenderer_present(Renderer* renderer) {
    WiiRenderer* r = (WiiRenderer*) renderer;

    // Anything still targeting a surface has to be folded back before the frame ends.
    if (r->currentTarget >= 0) {
        resolveTargetToTexture(r, r->currentTarget);
        r->currentTarget = RENDER_TARGET_HOST_FRAMEBUFFER;
    }
    r->dirty = false;
}
