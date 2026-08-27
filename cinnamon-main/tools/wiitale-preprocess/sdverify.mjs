#!/usr/bin/env node
// Reads files back out of a FAT32 image and compares them to their sources.
//
//   node sdverify.mjs <image.raw> <path-on-card> <src-dir>
//
// sdimage.mjs writes FAT structures by hand, so this walks the directory the way a real
// FAT driver would -- following the cluster chain, honouring long file names -- and
// checks every byte. If this passes, the failure is not in the image.

import fs from "node:fs";
import path from "node:path";

const ATTR_LFN = 0x0f;
const ATTR_DIRECTORY = 0x10;

// With --extract <name> <out>, pulls one file off the card instead of verifying. That is
// how the runner's own log gets back to the host: the Wii writes it to the SD image and
// there is no other way to read anything it printed after GX took over the screen.
const extractIdx = process.argv.indexOf("--extract");
const extract = extractIdx >= 0
    ? { name: process.argv[extractIdx + 1], out: process.argv[extractIdx + 2] }
    : null;

const [imagePath, cardPath, srcDir] = process.argv.slice(2);
const fd = fs.openSync(imagePath, "r");

const boot = Buffer.alloc(512);
fs.readSync(fd, boot, 0, 512, 0);
const bytesPerSector = boot.readUInt16LE(11);
const secPerClus = boot[13];
const rsvdSecCnt = boot.readUInt16LE(14);
const numFATs = boot[16];
const fatSize = boot.readUInt16LE(22) || boot.readUInt32LE(36);
const rootClus = boot.readUInt32LE(44);
const dataStart = rsvdSecCnt + numFATs * fatSize;
const clusterBytes = bytesPerSector * secPerClus;

const fat = Buffer.alloc(fatSize * bytesPerSector);
fs.readSync(fd, fat, 0, fat.length, rsvdSecCnt * bytesPerSector);
const nextCluster = (c) => fat.readUInt32LE(c * 4) & 0x0fffffff;

function clusterOffset(c) {
    return (dataStart + (c - 2) * secPerClus) * bytesPerSector;
}

function chain(start) {
    const out = [];
    let c = start;
    while (c >= 2 && c < 0x0ffffff8 && out.length < 2_000_000) {
        out.push(c);
        c = nextCluster(c);
    }
    return out;
}

function readChain(start, size) {
    const out = Buffer.alloc(size);
    let done = 0;
    for (const c of chain(start)) {
        if (done >= size) break;
        const n = Math.min(clusterBytes, size - done);
        fs.readSync(fd, out, done, n, clusterOffset(c));
        done += n;
    }
    return out;
}

// Reassembles long file names from the LFN records that precede each short entry.
function listDir(startCluster) {
    const items = [];
    let lfnParts = [];

    for (const c of chain(startCluster)) {
        const buf = Buffer.alloc(clusterBytes);
        fs.readSync(fd, buf, 0, clusterBytes, clusterOffset(c));

        for (let o = 0; o + 32 <= clusterBytes; o += 32) {
            const first = buf[o];
            if (first === 0x00) return items;
            if (first === 0xe5) { lfnParts = []; continue; }

            const attr = buf[o + 11];
            if (attr === ATTR_LFN) {
                const idx = (buf[o] & 0x3f) - 1;
                let s = "";
                const grab = (at) => {
                    const v = buf.readUInt16LE(at);
                    if (v !== 0x0000 && v !== 0xffff) s += String.fromCharCode(v);
                };
                for (let i = 0; i < 5; i++) grab(o + 1 + i * 2);
                for (let i = 0; i < 6; i++) grab(o + 14 + i * 2);
                for (let i = 0; i < 2; i++) grab(o + 28 + i * 2);
                lfnParts[idx] = s;
                continue;
            }

            const shortName = buf.toString("latin1", o, o + 11);
            // A name that already fits 8.3 gets no long-name records, so it has to be
            // rebuilt from the padded "NAME    EXT" form rather than just trimmed.
            const base83 = shortName.slice(0, 8).trim();
            const ext83 = shortName.slice(8, 11).trim();
            const name = lfnParts.length
                ? lfnParts.join("")
                : (ext83 ? `${base83}.${ext83}` : base83);
            lfnParts = [];
            if (shortName[0] === ".") continue;

            items.push({
                name,
                shortName,
                isDir: (attr & ATTR_DIRECTORY) !== 0,
                cluster: (buf.readUInt16LE(o + 20) << 16) | buf.readUInt16LE(o + 26),
                size: buf.readUInt32LE(o + 28),
            });
        }
    }
    return items;
}

let dir = rootClus;
for (const part of cardPath.split("/").filter(Boolean)) {
    const hit = listDir(dir).find((e) => e.isDir && e.name.toLowerCase() === part.toLowerCase());
    if (!hit) { console.error(`FAIL: directory ${part} not found on the card`); process.exit(1); }
    dir = hit.cluster;
}

const entries = listDir(dir).filter((e) => !e.isDir);

if (extract) {
    const hit = entries.find((e) => e.name.toLowerCase() === extract.name.toLowerCase());
    if (!hit) {
        console.error(`FAIL: ${extract.name} is not on the card`);
        console.error(`present: ${entries.map((e) => e.name).join(", ")}`);
        process.exit(1);
    }
    fs.writeFileSync(extract.out, readChain(hit.cluster, hit.size));
    console.log(`extracted ${hit.name} (${hit.size} bytes) -> ${extract.out}`);
    fs.closeSync(fd);
    process.exit(0);
}

console.log(`${cardPath} holds ${entries.length} files\n`);

let ok = 0, bad = 0;
for (const e of entries) {
    const src = path.join(srcDir, e.name);
    const got = readChain(e.cluster, e.size);

    if (!fs.existsSync(src)) {
        console.log(`  ${e.name.padEnd(20)} on card only, nothing to compare`);
        continue;
    }
    const want = fs.readFileSync(src);
    const sizeOk = want.length === e.size;
    const dataOk = sizeOk && got.equals(want);

    console.log(`  ${e.name.padEnd(20)} ${String(e.size).padStart(10)} B  ` +
                `short=${e.shortName}  ${dataOk ? "identical" : sizeOk ? "CONTENT DIFFERS" : `SIZE ${want.length} != ${e.size}`}`);
    if (dataOk) ok++; else bad++;
}

console.log(`\n${ok} file(s) verified byte for byte, ${bad} bad`);
fs.closeSync(fd);
process.exit(bad === 0 ? 0 : 1);
