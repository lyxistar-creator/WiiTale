#!/usr/bin/env node
// One-off survey of the AUDO chunk: what the embedded sounds actually are, so the
// packer's conversion targets are chosen from data rather than assumption.
import fs from "node:fs";

const buf = fs.readFileSync(process.argv[2]);

function chunk(name) {
    let p = 8;
    while (p + 8 <= buf.length) {
        const n = buf.toString("latin1", p, p + 4);
        const len = buf.readUInt32LE(p + 4);
        if (n === name) return { offset: p + 8, length: len };
        p += 8 + len;
    }
    return null;
}

const audo = chunk("AUDO");
const count = buf.readUInt32LE(audo.offset);
const formats = new Map();
let totalBytes = 0, totalSeconds = 0, nonPcm = 0;
let maxBytes = 0, maxIdx = -1;

for (let i = 0; i < count; i++) {
    const ptr = buf.readUInt32LE(audo.offset + 4 + i * 4);
    const len = buf.readUInt32LE(ptr);
    const data = buf.subarray(ptr + 4, ptr + 4 + len);
    totalBytes += len;
    if (len > maxBytes) { maxBytes = len; maxIdx = i; }

    if (data.toString("latin1", 0, 4) !== "RIFF") { nonPcm++; continue; }

    // Walk the RIFF chunks to fmt and data.
    let p = 12, fmt = null, dataLen = 0;
    while (p + 8 <= data.length) {
        const id = data.toString("latin1", p, p + 4);
        const sz = data.readUInt32LE(p + 4);
        if (id === "fmt ") {
            fmt = {
                tag: data.readUInt16LE(p + 8),
                channels: data.readUInt16LE(p + 10),
                rate: data.readUInt32LE(p + 12),
                bits: data.readUInt16LE(p + 22),
            };
        } else if (id === "data") {
            dataLen = sz;
        }
        p += 8 + sz + (sz & 1);
    }
    if (!fmt) { nonPcm++; continue; }

    const key = `tag=${fmt.tag} ${fmt.rate}Hz ${fmt.channels}ch ${fmt.bits}bit`;
    formats.set(key, (formats.get(key) || 0) + 1);
    totalSeconds += dataLen / (fmt.rate * fmt.channels * (fmt.bits / 8));
}

console.log(`AUDO: ${count} entries, ${(totalBytes / 1024 / 1024).toFixed(1)} MB, ~${totalSeconds.toFixed(0)}s of audio`);
console.log(`largest entry: #${maxIdx} at ${(maxBytes / 1024).toFixed(0)} KB`);
console.log(`unparsed/non-RIFF: ${nonPcm}`);
console.log("formats:");
for (const [k, v] of [...formats].sort((a, b) => b[1] - a[1])) console.log(`  ${String(v).padStart(4)} x  ${k}`);
