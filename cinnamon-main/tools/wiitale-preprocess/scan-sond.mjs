#!/usr/bin/env node
// Survey of the SOND chunk: how each sound is stored, so the audio backend knows which
// ones come from the embedded pack and which are streamed from external files.
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

// STRG strings are referenced by a pointer that lands on the character data itself,
// with the length stored in the four bytes before it.
function str(ptr) {
    if (ptr === 0) return "";
    const len = buf.readUInt32LE(ptr - 4);
    return buf.toString("latin1", ptr, ptr + len);
}

const sond = chunk("SOND");
const count = buf.readUInt32LE(sond.offset);
console.log(`SOND: ${count} sounds\n`);

let embedded = 0, external = 0, compressed = 0;
const samples = [];

for (let i = 0; i < count; i++) {
    const ptr = buf.readUInt32LE(sond.offset + 4 + i * 4);
    const name = str(buf.readUInt32LE(ptr));
    const flags = buf.readUInt32LE(ptr + 4);
    const type = str(buf.readUInt32LE(ptr + 8));
    const file = str(buf.readUInt32LE(ptr + 12));
    const audioFile = buf.readInt32LE(ptr + 32);

    // flags bit 0 = embedded. audioFile >= 0 means the samples live in AUDO; otherwise the runner has to open
    // the file named in `file` next to the game.
    if (audioFile >= 0) embedded++; else external++;
    if (flags & 0x02) compressed++;

    if (samples.length < 8) samples.push({ i, name, flags, type, file, audioFile });
}

console.log(`embedded (AUDO):  ${embedded}`);
console.log(`external (file):  ${external}`);
console.log(`flagged compressed: ${compressed}\n`);
console.log("first few entries:");
for (const s of samples) {
    console.log(`  [${String(s.i).padStart(3)}] flags=0x${s.flags.toString(16).padStart(2, "0")} audioFile=${String(s.audioFile).padStart(4)} type=${JSON.stringify(s.type)} file=${JSON.stringify(s.file)}`);
}

// How many distinct external file extensions are referenced.
const exts = new Map();
for (let i = 0; i < count; i++) {
    const ptr = buf.readUInt32LE(sond.offset + 4 + i * 4);
    const file = str(buf.readUInt32LE(ptr + 12));
    const m = file.match(/\.([a-z0-9]+)$/i);
    const k = m ? m[1].toLowerCase() : "(none)";
    exts.set(k, (exts.get(k) || 0) + 1);
}
console.log("\nreferenced file extensions:");
for (const [k, v] of [...exts].sort((a, b) => b[1] - a[1])) console.log(`  ${String(v).padStart(4)} x .${k}`);
