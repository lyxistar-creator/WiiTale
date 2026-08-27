#!/usr/bin/env node
// Generates the Homebrew Channel icon.
//
//   node makeicon.mjs <out.png>
//
// The channel wants a 128x48 PNG. Writing one by hand is easier than pulling in an image
// library: PNG is a handful of chunks around a zlib stream, and Node has zlib built in.

import fs from "node:fs";
import zlib from "node:zlib";

const W = 128, H = 48;

// ---- palette ----
const BG_TOP = [16, 12, 28];
const BG_BOT = [40, 20, 48];
const HEART  = [255, 40, 60];
const HEART_DARK = [170, 20, 40];
const WHITE  = [235, 235, 245];

const px = Buffer.alloc(W * H * 4);
const set = (x, y, [r, g, b], a = 255) => {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    const o = (y * W + x) * 4;
    px[o] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = a;
};

// Vertical gradient background.
for (let y = 0; y < H; y++) {
    const t = y / (H - 1);
    const c = [0, 1, 2].map((i) => Math.round(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t));
    for (let x = 0; x < W; x++) set(x, y, c);
}

// The soul: the one shape that reads as this game at any size. Drawn from a small mask
// and scaled up, so it keeps hard pixel edges instead of going blurry.
const HEART_MASK = [
    "..XXX...XXX..",
    ".XXXXX.XXXXX.",
    "XXXXXXXXXXXXX",
    "XXXXXXXXXXXXX",
    "XXXXXXXXXXXXX",
    ".XXXXXXXXXXX.",
    "..XXXXXXXXX..",
    "...XXXXXXX...",
    "....XXXXX....",
    ".....XXX.....",
    "......X......",
];
const SCALE = 3;
const hw = HEART_MASK[0].length * SCALE;
const hh = HEART_MASK.length * SCALE;
const hx = 12;
const hy = Math.floor((H - hh) / 2);

for (let my = 0; my < HEART_MASK.length; my++) {
    for (let mx = 0; mx < HEART_MASK[my].length; mx++) {
        if (HEART_MASK[my][mx] !== "X") continue;
        // A darker rim on the lower right gives the flat shape a little depth.
        const edge = (my === HEART_MASK.length - 1) ||
                     (mx + 1 < HEART_MASK[my].length && HEART_MASK[my][mx + 1] !== "X");
        const col = edge ? HEART_DARK : HEART;
        for (let sy = 0; sy < SCALE; sy++) {
            for (let sx = 0; sx < SCALE; sx++) set(hx + mx * SCALE + sx, hy + my * SCALE + sy, col);
        }
    }
}

// "WiiTale" in a tiny hand-built 5x7 font. Only the letters this word needs exist.
const GLYPHS = {
    W: ["X...X", "X...X", "X...X", "X.X.X", "X.X.X", "XX.XX", "X...X"],
    i: [".....", "..X..", ".....", "..X..", "..X..", "..X..", "..X.."],
    T: ["XXXXX", "..X..", "..X..", "..X..", "..X..", "..X..", "..X.."],
    a: [".....", ".....", ".XXX.", "....X", ".XXXX", "X...X", ".XXXX"],
    l: ["..X..", "..X..", "..X..", "..X..", "..X..", "..X..", "..X.."],
    e: [".....", ".....", ".XXX.", "X...X", "XXXXX", "X....", ".XXX."],
};

function drawText(text, ox, oy, scale, colour) {
    let cx = ox;
    for (const ch of text) {
        const g = GLYPHS[ch];
        if (!g) { cx += 3 * scale; continue; }
        for (let y = 0; y < g.length; y++) {
            for (let x = 0; x < g[y].length; x++) {
                if (g[y][x] !== "X") continue;
                for (let sy = 0; sy < scale; sy++)
                    for (let sx = 0; sx < scale; sx++)
                        set(cx + x * scale + sx, oy + y * scale + sy, colour);
            }
        }
        cx += (g[0].length + 1) * scale;
    }
}

drawText("WiiTale", hx + hw + 10, 17, 2, WHITE);

// ---- PNG encoding ----
function chunk(type, data) {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type, "latin1"), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body) >>> 0);
    return Buffer.concat([len, body, crc]);
}

let crcTable = null;
function crc32(buf) {
    if (!crcTable) {
        crcTable = new Int32Array(256);
        for (let n = 0; n < 256; n++) {
            let c = n;
            for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
            crcTable[n] = c;
        }
    }
    let c = 0xffffffff;
    for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
    return c ^ 0xffffffff;
}

const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(W, 0);
ihdr.writeUInt32BE(H, 4);
ihdr[8] = 8;   // bit depth
ihdr[9] = 6;   // colour type: RGBA
// 10..12 stay zero: deflate, adaptive filtering, no interlace

// Each scanline is prefixed with its filter type; 0 (none) keeps this simple and the
// image is tiny, so the extra bytes cost nothing.
const raw = Buffer.alloc(H * (1 + W * 4));
for (let y = 0; y < H; y++) {
    raw[y * (1 + W * 4)] = 0;
    px.copy(raw, y * (1 + W * 4) + 1, y * W * 4, (y + 1) * W * 4);
}

const png = Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", zlib.deflateSync(raw, { level: 9 })),
    chunk("IEND", Buffer.alloc(0)),
]);

const out = process.argv[2] || "icon.png";
fs.writeFileSync(out, png);
console.log(`wrote ${out} (${W}x${H}, ${png.length} bytes)`);
