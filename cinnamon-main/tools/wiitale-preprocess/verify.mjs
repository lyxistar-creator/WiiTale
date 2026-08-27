#!/usr/bin/env node
// Round-trips the generated pack against the original data.win.
//
// The console cannot be tested from here, so this checks the two things that would
// otherwise only fail on hardware: that the pack's structure is exactly what
// src/wii/wii_textures.c reads, and that untiling a page and pushing its indices back
// through the TLUT reproduces the source image within quantisation error.
//
//   node verify.mjs <data.win> <pack.wtex>

import fs from "node:fs";
import { decodePng } from "./png.mjs";

const HEADER_SIZE = 12;
const TOC_ENTRY_SIZE = 16;
const PALETTE_SIZE = 256;

function encodeRGB5A3(r, g, b, a) {
    if (a >= 224) return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
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

function findTxtrEntries(buf) {
    let p = 8;
    while (p + 8 <= buf.length) {
        const name = buf.toString("latin1", p, p + 4);
        const len = buf.readUInt32LE(p + 4);
        if (name === "TXTR") {
            const off = p + 8;
            const count = buf.readUInt32LE(off);
            const out = [];
            for (let i = 0; i < count; i++) {
                const ptr = buf.readUInt32LE(off + 4 + i * 4);
                out.push(buf.readUInt32LE(ptr + 4));
            }
            return out;
        }
        p += 8 + len;
    }
    throw new Error("no TXTR chunk");
}

const [dataWinPath, packPath] = process.argv.slice(2);
const dw = fs.readFileSync(dataWinPath);
const pack = fs.readFileSync(packPath);

// ===[ structural checks, mirroring the C loader ]===
const magic = pack.readUInt32BE(0);
if (magic !== 0x57544558) throw new Error(`bad magic 0x${magic.toString(16)}`);
const version = pack.readUInt16BE(4);
const pageCount = pack.readUInt16BE(6);
const tlutCount = pack.readUInt16BE(8);
console.log(`pack: version ${version}, ${pageCount} pages, ${tlutCount} TLUTs, ${(pack.length / 1024 / 1024).toFixed(1)} MB`);

const tlutBase = HEADER_SIZE + pageCount * TOC_ENTRY_SIZE;
let structureOk = true;
const pages = [];
for (let i = 0; i < pageCount; i++) {
    const o = HEADER_SIZE + i * TOC_ENTRY_SIZE;
    const page = {
        width: pack.readUInt16BE(o),
        height: pack.readUInt16BE(o + 2),
        tlutIndex: pack.readUInt16BE(o + 4),
        format: pack.readUInt16BE(o + 6) & 1,   // 0 = CI8, 1 = RGB5A3
        dataOffset: pack.readUInt32BE(o + 8),
        dataSize: pack.readUInt32BE(o + 12),
    };
    pages.push(page);

    // Tile geometry differs by format: CI8 is 8x4 texels per 32-byte tile, RGB5A3 is 4x4.
    const paddedW = page.format === 1 ? ((page.width + 3) & ~3) : ((page.width + 7) & ~7);
    const paddedH = (page.height + 3) & ~3;
    const expected = page.format === 1 ? paddedW * paddedH * 2 : paddedW * paddedH;
    if (page.dataSize !== expected) {
        console.log(`  page ${i}: FAIL size ${page.dataSize} != ${expected}`);
        structureOk = false;
    }
    if (page.dataOffset + page.dataSize > pack.length) {
        console.log(`  page ${i}: FAIL payload runs past end of file`);
        structureOk = false;
    }
    // The MEM2 pool allocates in power-of-two aligned runs; a page that is not a power
    // of two would still work but would waste a whole block.
    if ((page.dataSize & (page.dataSize - 1)) !== 0) {
        console.log(`  page ${i}: WARN size ${page.dataSize} is not a power of two`);
    }
}
console.log(`structure: ${structureOk ? "OK" : "FAILED"}`);

// ===[ pixel round-trip ]===
const pngOffsets = findTxtrEntries(dw);
let worstPage = -1, worstErr = -1;
let totalExact = 0;

for (let i = 0; i < pageCount; i++) {
    const page = pages[i];
    if (page.dataSize === 0) continue;

    const img = decodePng(dw, pngOffsets[i]);
    const isCI8 = page.format === 0;
    const paddedW = isCI8 ? ((page.width + 7) & ~7) : ((page.width + 3) & ~3);
    const tilesPerRow = isCI8 ? (paddedW >> 3) : (paddedW >> 2);

    // Rebuild the palette exactly as GX would read it.
    const pal = [];
    for (let e = 0; e < PALETTE_SIZE; e++) {
        pal.push(decodeRGB5A3(pack.readUInt16BE(tlutBase + (page.tlutIndex * PALETTE_SIZE + e) * 2)));
    }

    let sumSq = 0, exact = 0;
    const count = img.width * img.height;
    for (let y = 0; y < img.height; y++) {
        for (let x = 0; x < img.width; x++) {
            let got;
            if (isCI8) {
                const ty = y >> 2, iy = y & 3, tx = x >> 3, ix = x & 7;
                got = pal[pack[page.dataOffset + ((ty * tilesPerRow + tx) << 5) + (iy << 3) + ix]];
            } else {
                const ty = y >> 2, iy = y & 3, tx = x >> 2, ix = x & 3;
                const at = page.dataOffset + ((ty * tilesPerRow + tx) << 5) + ((iy << 2) + ix) * 2;
                got = decodeRGB5A3(pack.readUInt16BE(at));
            }

            const s = (y * img.width + x) * 4;
            const want = decodeRGB5A3(encodeRGB5A3(img.data[s], img.data[s + 1], img.data[s + 2], img.data[s + 3]));

            // A fully transparent texel carries no visible colour: the preprocessor
            // collapses every such texel onto palette index 0, and with point sampling
            // (GX_NEAR) its RGB is never fetched. Comparing those channels would report
            // a huge error for something that cannot be seen, so only alpha is checked.
            let d;
            if (got[3] === 0 && want[3] === 0) {
                d = 0;
            } else {
                const dr = got[0] - want[0], dg = got[1] - want[1];
                const db = got[2] - want[2], da = got[3] - want[3];
                d = dr * dr + dg * dg + db * db + da * da;
            }
            sumSq += d;
            if (d === 0) exact++;
        }
    }
    const rmse = Math.sqrt(sumSq / count);
    const pct = (exact / count) * 100;
    if (pct === 100) totalExact++;
    if (rmse > worstErr) { worstErr = rmse; worstPage = i; }

    console.log(`page ${String(i).padStart(2)}: ${pct.toFixed(2).padStart(6)}% texels exact, RMSE ${rmse.toFixed(2)}`);
}

console.log(`\n${totalExact}/${pageCount} pages reproduce the source exactly`);
console.log(`worst page is ${worstPage} at RMSE ${worstErr.toFixed(2)}`);
