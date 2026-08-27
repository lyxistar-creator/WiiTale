#!/usr/bin/env node
// wiitale-preprocess -- builds the Wii texture pack from a GameMaker data.win.
//
//   node preprocess.mjs <data.win> <output.wtex>
//
// Undertale's 26 texture pages are 188 MB decoded to RGBA8, against 88 MB of total Wii
// RAM, so the console never sees an RGBA page. Each page is quantised here to a
// 256-entry palette and stored as CI8 (one byte per texel) plus an RGB5A3 TLUT, already
// laid out in GX's tiled order. That is a quarter of the RGBA size, and it makes
// loading a page on the console a plain read with no decoding and no scratch memory.
//
// The output format is documented in src/wii/wii_textures.h.

import fs from "node:fs";
import { decodePng, pngLength } from "./png.mjs";

const PACK_MAGIC = 0x57544558; // 'WTEX'
const PACK_VERSION = 2;
const HEADER_SIZE = 16;
const PAGE_ENTRY_SIZE = 12;
const SUB_ENTRY_SIZE = 16;
const PALETTE_SIZE = 256;

// GX cannot address a texture larger than 1024 in either axis -- the header states the
// limit outright. GameMaker's atlas pages are up to 2048x2048, so every page is cut into
// a grid of tiles no larger than this, and the runtime reassembles sprites that span more
// than one. Anything above 1024 is simply unreachable by the hardware, which is why
// sprites packed low on a tall page rendered as garbage.
const MAX_TEXTURE_DIM = 1024;

// ===[ RGB5A3 ]===
// The Wii's 16bpp texture format: either 5:5:5 opaque, or 4:4:4 with 3 bits of alpha.
// The cutoff mirrors the hardware's own interpretation of the top bit.
function encodeRGB5A3(r, g, b, a) {
    if (a >= 224) {
        return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    }
    return ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
}

function decodeRGB5A3(v) {
    if (v & 0x8000) {
        const r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return [(r * 255 / 31) | 0, (g * 255 / 31) | 0, (b * 255 / 31) | 0, 255];
    }
    const a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
    return [(r * 255 / 15) | 0, (g * 255 / 15) | 0, (b * 255 / 15) | 0, (a * 255 / 7) | 0];
}

// ===[ Palette generation ]===
//
// Quantisation happens *after* the conversion to RGB5A3, so the palette is exact in the
// space the hardware actually samples and there are only 65536 possible colours. That
// makes the histogram a flat array and the whole pass cheap.
//
// Fully transparent texels are pulled out first and always given index 0: blending them
// into a median-cut box would drag the surrounding colours toward black and leave a halo
// around every sprite.
function buildPalette(pixels16, histogram) {
    const unique = [];
    let hasTransparent = false;
    for (let v = 0; v < 65536; v++) {
        if (histogram[v] === 0) continue;
        // A3 == 0 in the non-opaque encoding means fully transparent.
        if ((v & 0x8000) === 0 && ((v >> 12) & 7) === 0) { hasTransparent = true; continue; }
        unique.push(v);
    }

    const reserved = hasTransparent ? 1 : 0;
    const budget = PALETTE_SIZE - reserved;
    const palette = new Uint16Array(PALETTE_SIZE);
    let exact = false;

    if (unique.length <= budget) {
        // The page already fits in a palette: this round-trip is lossless.
        exact = true;
        for (let i = 0; i < unique.length; i++) palette[reserved + i] = unique[i];
    } else {
        const boxes = [{ colors: unique }];
        while (boxes.length < budget) {
            // Split the box with the widest spread on any single channel.
            let best = -1, bestRange = -1, bestAxis = 0;
            for (let i = 0; i < boxes.length; i++) {
                const box = boxes[i];
                if (box.colors.length < 2) continue;
                const lo = [255, 255, 255, 255], hi = [0, 0, 0, 0];
                for (const v of box.colors) {
                    const c = decodeRGB5A3(v);
                    for (let k = 0; k < 4; k++) {
                        if (c[k] < lo[k]) lo[k] = c[k];
                        if (c[k] > hi[k]) hi[k] = c[k];
                    }
                }
                for (let k = 0; k < 4; k++) {
                    const range = hi[k] - lo[k];
                    if (range > bestRange) { bestRange = range; best = i; bestAxis = k; }
                }
            }
            if (best < 0) break; // every box is a single colour

            const box = boxes[best];
            box.colors.sort((x, y) => decodeRGB5A3(x)[bestAxis] - decodeRGB5A3(y)[bestAxis]);

            // Split at the weighted median so both halves carry a similar pixel count.
            const total = box.colors.reduce((s, v) => s + histogram[v], 0);
            let acc = 0, cut = 1;
            for (let i = 0; i < box.colors.length - 1; i++) {
                acc += histogram[box.colors[i]];
                if (acc * 2 >= total) { cut = i + 1; break; }
            }
            boxes.splice(best, 1,
                { colors: box.colors.slice(0, cut) },
                { colors: box.colors.slice(cut) });
        }

        for (let i = 0; i < boxes.length; i++) {
            let wr = 0, wg = 0, wb = 0, wa = 0, w = 0;
            for (const v of boxes[i].colors) {
                const n = histogram[v];
                const c = decodeRGB5A3(v);
                wr += c[0] * n; wg += c[1] * n; wb += c[2] * n; wa += c[3] * n; w += n;
            }
            if (w === 0) continue;
            palette[reserved + i] = encodeRGB5A3(
                Math.round(wr / w), Math.round(wg / w), Math.round(wb / w), Math.round(wa / w));
        }
    }

    // Nearest-entry lookup for every colour the page actually uses. Building it once
    // turns the per-texel mapping into a single array read.
    const lut = new Uint8Array(65536);
    const palRGBA = [];
    for (let i = 0; i < PALETTE_SIZE; i++) palRGBA.push(decodeRGB5A3(palette[i]));

    for (let v = 0; v < 65536; v++) {
        if (histogram[v] === 0) continue;
        if ((v & 0x8000) === 0 && ((v >> 12) & 7) === 0) { lut[v] = 0; continue; }

        let bestIdx = reserved, bestDist = Infinity;
        const c = decodeRGB5A3(v);
        for (let i = reserved; i < PALETTE_SIZE; i++) {
            const p = palRGBA[i];
            const dr = c[0] - p[0], dg = c[1] - p[1], db = c[2] - p[2], da = c[3] - p[3];
            // Alpha is weighted heavily: a colour match that gets opacity wrong is far
            // more visible than one that is slightly off-hue.
            const dist = dr * dr + dg * dg + db * db + da * da * 4;
            if (dist < bestDist) { bestDist = dist; bestIdx = i; }
        }
        lut[v] = bestIdx;
    }

    return { palette, lut, exact, uniqueCount: unique.length + reserved };
}

// A page whose palettised form is worse than this (root-mean-square error over the
// visible texels) is stored as raw RGB5A3 instead. Undertale's pixel art quantises
// perfectly; the pages that fail this test are the pre-rendered and gradient-heavy ones,
// where banding would be obvious. They cost twice the memory, which is affordable
// because only a few pages are affected and they are streamed rather than all resident.
const RGB5A3_ERROR_THRESHOLD = 8.0;

// ===[ GX tiling ]===
//
// Both tilers read a sub-rectangle out of the full page and emit one GX texture for it,
// so a page can be cut into tiles that respect the 1024 limit without re-decoding.

// RGB5A3 textures are stored as 4x4 texel tiles of 32 bytes, tiles in row-major order.
function tileRGB5A3(pixels16, pageW, srcX, srcY, width, height) {
    const paddedW = (width + 3) & ~3;
    const paddedH = (height + 3) & ~3;
    const out = Buffer.alloc(paddedW * paddedH * 2);

    const tilesPerRow = paddedW >> 2;
    for (let y = 0; y < paddedH; y++) {
        const ty = y >> 2, iy = y & 3;
        for (let x = 0; x < paddedW; x++) {
            const tx = x >> 2, ix = x & 3;
            const dst = ((ty * tilesPerRow + tx) << 5) + ((iy << 2) + ix) * 2;
            const v = (x < width && y < height) ? pixels16[(srcY + y) * pageW + (srcX + x)] : 0;
            out.writeUInt16BE(v, dst);
        }
    }
    return out;
}

// CI8 textures are stored as 8x4 texel tiles of 32 bytes, tiles in row-major order.
function tileCI8(indices, pageW, srcX, srcY, width, height) {
    const paddedW = (width + 7) & ~7;
    const paddedH = (height + 3) & ~3;
    const out = Buffer.alloc(paddedW * paddedH);

    const tilesPerRow = paddedW >> 3;
    for (let y = 0; y < paddedH; y++) {
        const ty = y >> 2, iy = y & 3;
        for (let x = 0; x < paddedW; x++) {
            const tx = x >> 3, ix = x & 7;
            const dst = ((ty * tilesPerRow + tx) << 5) + (iy << 3) + ix;
            // Padding texels fall outside the real image and are left transparent.
            out[dst] = (x < width && y < height) ? indices[(srcY + y) * pageW + (srcX + x)] : 0;
        }
    }
    return out;
}

// ===[ data.win ]===
function findTxtrChunk(buf) {
    if (buf.toString("latin1", 0, 4) !== "FORM") throw new Error("not a GameMaker WAD (no FORM)");
    let p = 8;
    while (p + 8 <= buf.length) {
        const name = buf.toString("latin1", p, p + 4);
        const len = buf.readUInt32LE(p + 4);
        if (name === "TXTR") return { offset: p + 8, length: len };
        p += 8 + len;
    }
    throw new Error("no TXTR chunk found");
}

function readTxtrEntries(buf) {
    const { offset } = findTxtrChunk(buf);
    const count = buf.readUInt32LE(offset);
    const pointers = [];
    for (let i = 0; i < count; i++) pointers.push(buf.readUInt32LE(offset + 4 + i * 4));

    // Each TextureEntry is { u32 scaled; u32 pngOffset } for bytecode 16/17.
    return pointers.map((ptr) => ({
        scaled: buf.readUInt32LE(ptr),
        pngOffset: buf.readUInt32LE(ptr + 4),
    }));
}

// ===[ main ]===
function main() {
    const [dataWinPath, outPath] = process.argv.slice(2);
    if (!dataWinPath || !outPath) {
        console.error("usage: node preprocess.mjs <data.win> <output.wtex>");
        process.exit(2);
    }

    console.log(`reading ${dataWinPath} ...`);
    const buf = fs.readFileSync(dataWinPath);
    const entries = readTxtrEntries(buf);
    console.log(`found ${entries.length} texture pages\n`);

    const pages = [];
    let exactCount = 0;

    for (let i = 0; i < entries.length; i++) {
        const { pngOffset } = entries[i];
        if (pngOffset === 0) {
            pages.push({ width: 0, height: 0, cols: 0, rows: 0,
                         palette: new Uint16Array(PALETTE_SIZE), subs: [] });
            console.log(`page ${String(i).padStart(2)}: empty`);
            continue;
        }

        const size = pngLength(buf, pngOffset);
        const img = decodePng(buf, pngOffset);

        // RGBA8 -> RGB5A3, building the histogram in the same pass.
        const count = img.width * img.height;
        const pixels16 = new Uint16Array(count);
        const histogram = new Uint32Array(65536);
        for (let p = 0, s = 0; p < count; p++, s += 4) {
            const v = encodeRGB5A3(img.data[s], img.data[s + 1], img.data[s + 2], img.data[s + 3]);
            pixels16[p] = v;
            histogram[v]++;
        }

        const { palette, lut, exact, uniqueCount } = buildPalette(pixels16, histogram);

        const indices = new Uint8Array(count);
        for (let p = 0; p < count; p++) indices[p] = lut[pixels16[p]];

        // Measure what the palette actually cost, over the texels that are visible.
        // Fully transparent texels are skipped: their colour is never sampled.
        let sumSq = 0, measured = 0;
        for (let p = 0; p < count; p++) {
            const want = decodeRGB5A3(pixels16[p]);
            if (want[3] === 0) continue;
            const got = decodeRGB5A3(palette[indices[p]]);
            const dr = got[0] - want[0], dg = got[1] - want[1];
            const db = got[2] - want[2], da = got[3] - want[3];
            sumSq += dr * dr + dg * dg + db * db + da * da;
            measured++;
        }
        const rmse = measured > 0 ? Math.sqrt(sumSq / measured) : 0;

        const useRaw = rmse > RGB5A3_ERROR_THRESHOLD;
        const format = useRaw ? 1 : 0; // 1 = WII_TEXFMT_RGB5A3, 0 = WII_TEXFMT_CI8
        const storedPalette = useRaw ? new Uint16Array(PALETTE_SIZE) : palette;
        if (!useRaw && exact) exactCount++;

        // Cut the page into tiles no larger than the hardware can address. Every tile
        // keeps the page's palette, so they can share a TLUT slot at runtime.
        const cols = Math.ceil(img.width / MAX_TEXTURE_DIM);
        const rows = Math.ceil(img.height / MAX_TEXTURE_DIM);
        const subs = [];
        let pageBytes = 0;

        for (let r = 0; r < rows; r++) {
            for (let c = 0; c < cols; c++) {
                const sx = c * MAX_TEXTURE_DIM;
                const sy = r * MAX_TEXTURE_DIM;
                const sw = Math.min(MAX_TEXTURE_DIM, img.width - sx);
                const sh = Math.min(MAX_TEXTURE_DIM, img.height - sy);
                const data = useRaw
                    ? tileRGB5A3(pixels16, img.width, sx, sy, sw, sh)
                    : tileCI8(indices, img.width, sx, sy, sw, sh);
                subs.push({ width: sw, height: sh, format, data });
                pageBytes += data.length;
            }
        }

        pages.push({ width: img.width, height: img.height, cols, rows, palette: storedPalette, subs });

        const kind = useRaw ? "rgb5a3" : "ci8   ";
        const note = useRaw ? `(rgb5a3, palette rmse ${rmse.toFixed(1)})`
                            : (exact ? "(exact)" : `(quantised, rmse ${rmse.toFixed(1)})`);
        console.log(
            `page ${String(i).padStart(2)}: ${String(img.width).padStart(4)}x${String(img.height).padEnd(4)}` +
            ` png ${String((size / 1024) | 0).padStart(5)} KB -> ${kind} ${String((pageBytes / 1024) | 0).padStart(5)} KB` +
            ` in ${cols}x${rows} tile(s)  colours ${String(uniqueCount).padStart(5)}  ${note}`);
    }

    // ===[ write the pack ]===
    const pageCount = pages.length;
    const subCount = pages.reduce((n, p) => n + p.subs.length, 0);

    const pageTableSize = pageCount * PAGE_ENTRY_SIZE;
    const subTableSize = subCount * SUB_ENTRY_SIZE;
    const tlutSize = pageCount * PALETTE_SIZE * 2;
    let payloadOffset = HEADER_SIZE + pageTableSize + subTableSize + tlutSize;

    const header = Buffer.alloc(HEADER_SIZE);
    header.writeUInt32BE(PACK_MAGIC, 0);
    header.writeUInt16BE(PACK_VERSION, 4);
    header.writeUInt16BE(pageCount, 6);
    header.writeUInt16BE(subCount, 8);
    header.writeUInt16BE(pageCount, 10); // one TLUT per page, shared by its tiles

    const pageTable = Buffer.alloc(pageTableSize);
    const subTable = Buffer.alloc(subTableSize);
    const tluts = Buffer.alloc(tlutSize);
    const payloads = [];

    let subIndex = 0;
    for (let i = 0; i < pageCount; i++) {
        const page = pages[i];
        const o = i * PAGE_ENTRY_SIZE;
        pageTable.writeUInt16BE(page.width, o);
        pageTable.writeUInt16BE(page.height, o + 2);
        pageTable.writeUInt16BE(page.cols, o + 4);
        pageTable.writeUInt16BE(page.rows, o + 6);
        pageTable.writeUInt16BE(subIndex, o + 8);   // first tile
        pageTable.writeUInt16BE(i, o + 10);         // tlutIndex

        for (const sub of page.subs) {
            const so = subIndex * SUB_ENTRY_SIZE;
            subTable.writeUInt16BE(sub.width, so);
            subTable.writeUInt16BE(sub.height, so + 2);
            subTable.writeUInt16BE(sub.format, so + 4);
            subTable.writeUInt16BE(0, so + 6);
            subTable.writeUInt32BE(payloadOffset, so + 8);
            subTable.writeUInt32BE(sub.data.length, so + 12);

            payloads.push(sub.data);
            payloadOffset += sub.data.length;
            subIndex++;
        }

        for (let e = 0; e < PALETTE_SIZE; e++) {
            tluts.writeUInt16BE(page.palette[e], (i * PALETTE_SIZE + e) * 2);
        }
    }

    fs.writeFileSync(outPath, Buffer.concat([header, pageTable, subTable, tluts, ...payloads]));

    const total = payloadOffset;
    const largest = Math.max(...pages.flatMap((p) => p.subs.map((s) => s.data.length)));
    console.log(`\nwrote ${outPath}`);
    console.log(`  ${pageCount} pages split into ${subCount} tiles, ${(total / 1024 / 1024).toFixed(1)} MB total`);
    console.log(`  ${exactCount}/${pageCount} pages palettised losslessly`);
    console.log(`  largest tile ${(largest / 1024 / 1024).toFixed(2)} MB, at most ${MAX_TEXTURE_DIM}x${MAX_TEXTURE_DIM}`);
}

main();
