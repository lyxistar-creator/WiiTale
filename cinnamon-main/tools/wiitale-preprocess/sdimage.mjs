#!/usr/bin/env node
// Writes files into an existing FAT32 image.
//
//   node sdimage.mjs <image.raw> <src-dir> <dest-path-on-card>
//
// Dolphin's "sync folder to SD card" does not run here: it creates a blank card and
// leaves it empty. Rather than fight that, this drops the files straight into the image
// Dolphin already made. Reusing its image instead of formatting a new one means the
// geometry is known-good -- libfat mounts it, which is exactly what we need to keep.
//
// Only what is needed to place a handful of large files is implemented: directory
// creation, long file names, and cluster chains. There is no deletion and no
// fragmentation handling; files are written into fresh clusters.

import fs from "node:fs";
import path from "node:path";

const ATTR_DIRECTORY = 0x10;
const ATTR_ARCHIVE = 0x20;
const ATTR_LFN = 0x0f;

function readBPB(fd) {
    const b = Buffer.alloc(512);
    fs.readSync(fd, b, 0, 512, 0);
    if (b.readUInt16LE(510) !== 0xaa55) throw new Error("no boot signature: not a FAT image");

    const bpb = {
        bytesPerSector: b.readUInt16LE(11),
        secPerClus: b[13],
        rsvdSecCnt: b.readUInt16LE(14),
        numFATs: b[16],
        totSec16: b.readUInt16LE(19),
        fatSz16: b.readUInt16LE(22),
        totSec32: b.readUInt32LE(32),
        fatSz32: b.readUInt32LE(36),
        rootClus: b.readUInt32LE(44),
    };
    bpb.fatSize = bpb.fatSz16 !== 0 ? bpb.fatSz16 : bpb.fatSz32;
    bpb.totalSectors = bpb.totSec16 !== 0 ? bpb.totSec16 : bpb.totSec32;
    bpb.fatStart = bpb.rsvdSecCnt;
    bpb.dataStart = bpb.rsvdSecCnt + bpb.numFATs * bpb.fatSize;
    bpb.clusterBytes = bpb.bytesPerSector * bpb.secPerClus;
    bpb.clusterCount = Math.floor((bpb.totalSectors - bpb.dataStart) / bpb.secPerClus);
    return bpb;
}

class Fat32 {
    constructor(imagePath) {
        this.fd = fs.openSync(imagePath, "r+");
        this.bpb = readBPB(this.fd);
        // The whole FAT is held in memory and flushed once: it is a couple of MB, and
        // rewriting it per allocation would mean millions of tiny writes.
        this.fatBytes = this.bpb.fatSize * this.bpb.bytesPerSector;
        this.fat = Buffer.alloc(this.fatBytes);
        fs.readSync(this.fd, this.fat, 0, this.fatBytes, this.bpb.fatStart * this.bpb.bytesPerSector);
        this.searchHint = 2;
    }

    getFat(c) { return this.fat.readUInt32LE(c * 4) & 0x0fffffff; }
    setFat(c, v) {
        const old = this.fat.readUInt32LE(c * 4) & 0xf0000000;
        this.fat.writeUInt32LE((old | (v & 0x0fffffff)) >>> 0, c * 4);
    }

    flushFat() {
        for (let i = 0; i < this.bpb.numFATs; i++) {
            const at = (this.bpb.fatStart + i * this.bpb.fatSize) * this.bpb.bytesPerSector;
            fs.writeSync(this.fd, this.fat, 0, this.fatBytes, at);
        }
    }

    clusterOffset(c) {
        return (this.bpb.dataStart + (c - 2) * this.bpb.secPerClus) * this.bpb.bytesPerSector;
    }

    allocCluster() {
        for (let c = this.searchHint; c < this.bpb.clusterCount + 2; c++) {
            if (this.getFat(c) === 0) {
                this.setFat(c, 0x0ffffff8);
                this.searchHint = c + 1;
                return c;
            }
        }
        throw new Error("SD image is full");
    }

    readCluster(c) {
        const b = Buffer.alloc(this.bpb.clusterBytes);
        fs.readSync(this.fd, b, 0, b.length, this.clusterOffset(c));
        return b;
    }

    writeCluster(c, buf) {
        fs.writeSync(this.fd, buf, 0, this.bpb.clusterBytes, this.clusterOffset(c));
    }

    chainOf(startCluster) {
        const out = [];
        let c = startCluster;
        while (c >= 2 && c < 0x0ffffff8) {
            out.push(c);
            c = this.getFat(c);
        }
        return out;
    }

    // ---- directory handling ----

    *entries(dirCluster) {
        for (const c of this.chainOf(dirCluster)) {
            const buf = this.readCluster(c);
            for (let o = 0; o + 32 <= buf.length; o += 32) {
                yield { cluster: c, offset: o, buf };
            }
        }
    }

    // Full listing of a directory, with long names reassembled.
    //
    // Short names alone cannot identify a file here: the DOS mangling keeps only six
    // characters plus a "~1" tail, and this game ships dozens of names sharing a prefix
    // ("mus_ruins", "mus_rain", ...). Matching on the short name made those files collide
    // and silently overwrite one another, so identity comes from the long name and the
    // short name only has to be unique.
    listEntries(dirCluster) {
        const out = [];
        let lfn = [];
        for (const e of this.entries(dirCluster)) {
            const first = e.buf[e.offset];
            if (first === 0x00) break;
            if (first === 0xe5) { lfn = []; continue; }

            const attr = e.buf[e.offset + 11];
            if (attr === ATTR_LFN) {
                const idx = (first & 0x3f) - 1;
                let s = "";
                const grab = (at) => {
                    const v = e.buf.readUInt16LE(at);
                    if (v !== 0x0000 && v !== 0xffff) s += String.fromCharCode(v);
                };
                for (let i = 0; i < 5; i++) grab(e.offset + 1 + i * 2);
                for (let i = 0; i < 6; i++) grab(e.offset + 14 + i * 2);
                for (let i = 0; i < 2; i++) grab(e.offset + 28 + i * 2);
                lfn[idx] = s;
                continue;
            }

            const shortName = e.buf.toString("latin1", e.offset, e.offset + 11);
            const base = shortName.slice(0, 8).trim();
            const ext = shortName.slice(8, 11).trim();
            out.push({
                longName: lfn.length ? lfn.join("") : (ext ? `${base}.${ext}` : base),
                shortName,
                attr,
                cluster: (e.buf.readUInt16LE(e.offset + 20) << 16) | e.buf.readUInt16LE(e.offset + 26),
                at: { cluster: e.cluster, offset: e.offset },
            });
            lfn = [];
        }
        return out;
    }

    findByName(dirCluster, name) {
        const wanted = name.toLowerCase();
        return this.listEntries(dirCluster).find((e) => e.longName.toLowerCase() === wanted) || null;
    }

    // Returns a cluster chain to the free pool. Used when a file is replaced: without
    // this, rewriting a file would strand its old clusters and fill the card.
    freeChain(startCluster) {
        for (const c of this.chainOf(startCluster)) this.setFat(c, 0);
        if (startCluster >= 2 && startCluster < this.searchHint) this.searchHint = startCluster;
    }

    rewriteEntry(at, cluster, size) {
        const buf = this.readCluster(at.cluster);
        buf.writeUInt16LE((cluster >>> 16) & 0xffff, at.offset + 20);
        buf.writeUInt16LE(cluster & 0xffff, at.offset + 26);
        buf.writeUInt32LE(size >>> 0, at.offset + 28);
        this.writeCluster(at.cluster, buf);
    }

    // Appends 32-byte records at the end of the directory, growing it by whole clusters.
    //
    // The records for one file (its long-name entries followed by the short entry) must
    // be consecutive, but they may cross a cluster boundary. An earlier version bailed
    // out to a fresh cluster whenever the run did not fit in what was left of the current
    // one, which left a 0x00 slot behind -- and 0x00 means "end of directory" to every
    // FAT reader, so everything written afterwards became invisible. Appending strictly
    // at the end, and spilling into the next cluster mid-run, cannot leave such a hole.
    appendEntries(dirCluster, records) {
        const need = records.length;
        const perCluster = this.bpb.clusterBytes / 32;
        let chain = this.chainOf(dirCluster);

        // Find the end of the directory: the first slot that has never been used.
        let endSlot = chain.length * perCluster;
        outer:
        for (let ci = 0; ci < chain.length; ci++) {
            const buf = this.readCluster(chain[ci]);
            for (let i = 0; i < perCluster; i++) {
                if (buf[i * 32] === 0x00) { endSlot = ci * perCluster + i; break outer; }
            }
        }

        // Grow until the run fits contiguously from endSlot.
        while (endSlot + need > chain.length * perCluster) {
            const tail = chain[chain.length - 1];
            const extra = this.allocCluster();
            this.setFat(tail, extra);
            this.writeCluster(extra, Buffer.alloc(this.bpb.clusterBytes));
            chain = this.chainOf(dirCluster);
        }

        const slots = [];
        for (let k = 0; k < need; k++) {
            const abs = endSlot + k;
            slots.push({ c: chain[Math.floor(abs / perCluster)], i: abs % perCluster });
        }
        this.placeEntries(slots, records);
    }

    placeEntries(slots, records) {
        const byCluster = new Map();
        for (let k = 0; k < records.length; k++) {
            const { c, i } = slots[k];
            if (!byCluster.has(c)) byCluster.set(c, this.readCluster(c));
            records[k].copy(byCluster.get(c), i * 32);
        }
        for (const [c, buf] of byCluster) this.writeCluster(c, buf);
    }
}

// ---- name handling ----

function shortNameBytes(name, ext) {
    const s = (name.toUpperCase() + "        ").slice(0, 8) + (ext.toUpperCase() + "   ").slice(0, 3);
    return Buffer.from(s, "latin1");
}

function lfnChecksum(shortBytes) {
    let sum = 0;
    for (let i = 0; i < 11; i++) sum = (((sum & 1) << 7) + (sum >> 1) + shortBytes[i]) & 0xff;
    return sum;
}

// Splits a long name into the 13-character-per-record LFN entries that precede the
// short entry, in the reverse order FAT stores them.
function lfnRecords(longName, shortBytes) {
    const chk = lfnChecksum(shortBytes);
    const chars = [];
    for (const ch of longName) chars.push(ch.charCodeAt(0));
    chars.push(0);
    while (chars.length % 13 !== 0) chars.push(0xffff);

    const total = chars.length / 13;
    const out = [];
    for (let n = total; n >= 1; n--) {
        const rec = Buffer.alloc(32);
        rec[0] = n | (n === total ? 0x40 : 0);
        rec[11] = ATTR_LFN;
        rec[13] = chk;
        const base = (n - 1) * 13;
        const put = (idx, at) => rec.writeUInt16LE(chars[base + idx], at);
        for (let i = 0; i < 5; i++) put(i, 1 + i * 2);
        for (let i = 0; i < 6; i++) put(5 + i, 14 + i * 2);
        for (let i = 0; i < 2; i++) put(11 + i, 28 + i * 2);
        out.push(rec);
    }
    return out;
}

function dirRecord(shortBytes, attr, cluster, size) {
    const rec = Buffer.alloc(32);
    shortBytes.copy(rec, 0);
    rec[11] = attr;
    rec.writeUInt16LE(0x21, 22);        // a fixed, valid time
    rec.writeUInt16LE(0x5a21, 24);      // a fixed, valid date
    rec.writeUInt16LE((cluster >>> 16) & 0xffff, 20);
    rec.writeUInt16LE(cluster & 0xffff, 26);
    rec.writeUInt32LE(size >>> 0, 28);
    return rec;
}

// Splits "textures.wtex" into a DOS name plus the long name it needs, if any.
//
// `taken` is the set of short names already in the directory. The numeric tail is bumped
// until the result is unused, because a short name is the only identity a plain FAT
// reader has and two files must never share one.
function planName(name, taken) {
    const dot = name.lastIndexOf(".");
    const base = dot > 0 ? name.slice(0, dot) : name;
    const ext = dot > 0 ? name.slice(dot + 1) : "";
    const fitsShort = base.length <= 8 && ext.length <= 3 &&
                      /^[A-Za-z0-9_\-]*$/.test(base) && /^[A-Za-z0-9]*$/.test(ext) &&
                      name === name.toUpperCase();

    if (fitsShort) {
        const bytes = shortNameBytes(base, ext);
        if (!taken || !taken.has(bytes.toString("latin1"))) {
            return { shortBytes: bytes, long: null };
        }
    }

    const stem = base.replace(/[^A-Za-z0-9_\-]/g, "").toUpperCase() || "FILE";
    for (let n = 1; n < 1000000; n++) {
        const tail = "~" + n;
        const bytes = shortNameBytes(stem.slice(0, 8 - tail.length) + tail, ext);
        if (!taken || !taken.has(bytes.toString("latin1"))) {
            return { shortBytes: bytes, long: name };
        }
    }
    throw new Error(`cannot find a free short name for ${name}`);
}

function takenShortNames(fat, dirCluster) {
    return new Set(fat.listEntries(dirCluster).map((e) => e.shortName));
}

function ensureDir(fat, parentCluster, name) {
    const existing = fat.findByName(parentCluster, name);
    if (existing && (existing.attr & ATTR_DIRECTORY)) return existing.cluster;

    const plan = planName(name, takenShortNames(fat, parentCluster));

    const cluster = fat.allocCluster();
    const buf = Buffer.alloc(fat.bpb.clusterBytes);
    dirRecord(shortNameBytes(".", ""), ATTR_DIRECTORY, cluster, 0).copy(buf, 0);
    dirRecord(shortNameBytes("..", ""), ATTR_DIRECTORY,
              parentCluster === fat.bpb.rootClus ? 0 : parentCluster, 0).copy(buf, 32);
    fat.writeCluster(cluster, buf);

    const records = [];
    if (plan.long) records.push(...lfnRecords(plan.long, plan.shortBytes));
    records.push(dirRecord(plan.shortBytes, ATTR_DIRECTORY, cluster, 0));
    fat.appendEntries(parentCluster, records);
    return cluster;
}

function writeFile(fat, dirCluster, name, data) {
    const clusterBytes = fat.bpb.clusterBytes;
    const needed = Math.max(1, Math.ceil(data.length / clusterBytes));

    // If the file is already on the card, reuse its directory record and give its old
    // clusters back first, so repeated writes do not duplicate entries or leak space.
    // Identity is the long name: short names are mangled and would confuse files that
    // share a prefix.
    const existing = fat.findByName(dirCluster, name);
    const replacing = existing && !(existing.attr & ATTR_DIRECTORY) ? existing : null;
    if (replacing) fat.freeChain(replacing.cluster);

    let first = 0, prev = 0;
    for (let i = 0; i < needed; i++) {
        const c = fat.allocCluster();
        if (i === 0) first = c; else fat.setFat(prev, c);
        prev = c;

        const start = i * clusterBytes;
        const slice = data.subarray(start, Math.min(start + clusterBytes, data.length));
        if (slice.length === clusterBytes) {
            fs.writeSync(fat.fd, slice, 0, clusterBytes, fat.clusterOffset(c));
        } else {
            const pad = Buffer.alloc(clusterBytes);
            slice.copy(pad, 0);
            fs.writeSync(fat.fd, pad, 0, clusterBytes, fat.clusterOffset(c));
        }
    }
    fat.setFat(prev, 0x0ffffff8);

    if (replacing) {
        fat.rewriteEntry(replacing.at, first, data.length);
        return "replaced";
    }

    const plan = planName(name, takenShortNames(fat, dirCluster));
    const records = [];
    if (plan.long) records.push(...lfnRecords(plan.long, plan.shortBytes));
    records.push(dirRecord(plan.shortBytes, ATTR_ARCHIVE, first, data.length));
    fat.appendEntries(dirCluster, records);
    return "added";
}

// ---- main ----
const [imagePath, srcDir, destPath] = process.argv.slice(2);
if (!imagePath || !srcDir || !destPath) {
    console.error("usage: node sdimage.mjs <image.raw> <src-dir> <dest-path-on-card>");
    process.exit(2);
}

const fat = new Fat32(imagePath);
const b = fat.bpb;
console.log(`image: ${(b.totalSectors * b.bytesPerSector / 1024 / 1024).toFixed(0)} MB, ` +
            `${b.bytesPerSector}B/sector, ${b.secPerClus} sectors/cluster ` +
            `(${b.clusterBytes}B clusters), ${b.numFATs} FATs, root at cluster ${b.rootClus}`);

let dir = b.rootClus;
for (const part of destPath.split("/").filter(Boolean)) {
    dir = ensureDir(fat, dir, part);
    console.log(`  dir ${part} -> cluster ${dir}`);
}

let written = 0;
for (const name of fs.readdirSync(srcDir).sort()) {
    const full = path.join(srcDir, name);
    if (!fs.statSync(full).isFile()) continue;
    const data = fs.readFileSync(full);
    const action = writeFile(fat, dir, name, data);
    written += data.length;
    console.log(`  ${name.padEnd(28)} ${(data.length / 1024 / 1024).toFixed(2).padStart(8)} MB  ${action}`);
}

fat.flushFat();
fs.closeSync(fat.fd);

const free = b.clusterCount - fat.searchHint;
console.log(`\nwrote ${(written / 1024 / 1024).toFixed(1)} MB into ${destPath}`);
console.log(`about ${(free * b.clusterBytes / 1024 / 1024).toFixed(0)} MB of the card still free`);
