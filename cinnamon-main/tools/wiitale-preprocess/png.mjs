// Minimal PNG decoder built on Node's bundled zlib.
//
// This exists so the preprocessor has no npm dependencies at all: the only thing it
// needs beyond the standard library is inflate, and Node ships that. Only what
// GameMaker actually writes into a TXTR chunk is supported -- 8-bit, non-interlaced --
// and anything else fails loudly rather than producing a subtly wrong atlas.

import zlib from "node:zlib";

const CHANNELS = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };

// Returns the exact byte length of the PNG starting at `offset`, by walking its chunk
// list to IEND. TXTR stores only a start offset per page, so the extent has to be
// recovered from the stream itself.
export function pngLength(buf, offset) {
    let p = offset + 8; // skip signature
    for (;;) {
        if (p + 8 > buf.length) throw new Error("PNG truncated while scanning chunks");
        const len = buf.readUInt32BE(p);
        const type = buf.toString("latin1", p + 4, p + 8);
        p += 12 + len; // length + type + data + crc
        if (type === "IEND") return p - offset;
    }
}

function paeth(a, b, c) {
    const p = a + b - c;
    const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// Decodes a PNG into a tightly packed RGBA8 buffer.
export function decodePng(buf, offset) {
    const sig = buf.readUInt32BE(offset);
    if (sig !== 0x89504e47) throw new Error("not a PNG (bad signature)");

    let width = 0, height = 0, bitDepth = 0, colorType = 0, interlace = 0;
    let palette = null, trns = null;
    const idat = [];

    let p = offset + 8;
    for (;;) {
        const len = buf.readUInt32BE(p);
        const type = buf.toString("latin1", p + 4, p + 8);
        const data = buf.subarray(p + 8, p + 8 + len);

        if (type === "IHDR") {
            width = data.readUInt32BE(0);
            height = data.readUInt32BE(4);
            bitDepth = data[8];
            colorType = data[9];
            interlace = data[12];
        } else if (type === "PLTE") {
            palette = Buffer.from(data);
        } else if (type === "tRNS") {
            trns = Buffer.from(data);
        } else if (type === "IDAT") {
            idat.push(Buffer.from(data));
        } else if (type === "IEND") {
            break;
        }
        p += 12 + len;
    }

    if (bitDepth !== 8) throw new Error(`unsupported bit depth ${bitDepth}`);
    if (interlace !== 0) throw new Error("interlaced PNG is not supported");
    const nch = CHANNELS[colorType];
    if (!nch) throw new Error(`unsupported colour type ${colorType}`);

    const raw = zlib.inflateSync(Buffer.concat(idat));

    const stride = width * nch;
    const out = Buffer.alloc(width * height * 4);
    const line = Buffer.alloc(stride);
    const prev = Buffer.alloc(stride);

    let src = 0;
    for (let y = 0; y < height; y++) {
        const filter = raw[src++];
        raw.copy(line, 0, src, src + stride);
        src += stride;

        // Un-filter in place. bpp is the byte distance to the pixel on the left.
        const bpp = nch;
        switch (filter) {
            case 0: break;
            case 1:
                for (let i = bpp; i < stride; i++) line[i] = (line[i] + line[i - bpp]) & 0xff;
                break;
            case 2:
                for (let i = 0; i < stride; i++) line[i] = (line[i] + prev[i]) & 0xff;
                break;
            case 3:
                for (let i = 0; i < stride; i++) {
                    const left = i >= bpp ? line[i - bpp] : 0;
                    line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xff;
                }
                break;
            case 4:
                for (let i = 0; i < stride; i++) {
                    const left = i >= bpp ? line[i - bpp] : 0;
                    const up = prev[i];
                    const ul = i >= bpp ? prev[i - bpp] : 0;
                    line[i] = (line[i] + paeth(left, up, ul)) & 0xff;
                }
                break;
            default:
                throw new Error(`unknown PNG filter ${filter} on row ${y}`);
        }
        line.copy(prev);

        // Expand whatever colour type this is into RGBA8.
        let d = y * width * 4;
        for (let x = 0; x < width; x++) {
            const s = x * nch;
            let r, g, b, a = 255;
            switch (colorType) {
                case 0: r = g = b = line[s]; break;
                case 2: r = line[s]; g = line[s + 1]; b = line[s + 2]; break;
                case 3: {
                    const idx = line[s];
                    r = palette[idx * 3]; g = palette[idx * 3 + 1]; b = palette[idx * 3 + 2];
                    if (trns && idx < trns.length) a = trns[idx];
                    break;
                }
                case 4: r = g = b = line[s]; a = line[s + 1]; break;
                case 6: r = line[s]; g = line[s + 1]; b = line[s + 2]; a = line[s + 3]; break;
            }
            out[d++] = r; out[d++] = g; out[d++] = b; out[d++] = a;
        }
    }

    return { width, height, data: out };
}
