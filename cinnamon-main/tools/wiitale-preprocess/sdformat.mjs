#!/usr/bin/env node
// Creates an empty FAT32 image that libfat will mount.
//
//   node sdformat.mjs <image.raw> <size-in-MB>
//
// Dolphin's own card is 128 MB, which cannot hold data.win, the texture pack and the
// game's 90 MB of music at once. Rather than depend on a setting that has to be changed
// by hand, the image is built here: superfloppy FAT32 with no partition table, matching
// the geometry Dolphin itself produces, which libfat is already known to accept.

import fs from "node:fs";

const [imagePath, sizeArg] = process.argv.slice(2);
if (!imagePath || !sizeArg) {
    console.error("usage: node sdformat.mjs <image.raw> <size-in-MB>");
    process.exit(2);
}

const BYTES_PER_SECTOR = 512;
const RESERVED_SECTORS = 32;
const NUM_FATS = 2;

const sizeMB = parseInt(sizeArg, 10);
const totalSectors = Math.floor((sizeMB * 1024 * 1024) / BYTES_PER_SECTOR);

// Cluster size by capacity, following the usual FAT32 breakpoints. Bigger clusters keep
// the FAT small; 4 KB is right for cards of this size.
const secPerClus = sizeMB <= 260 ? 1 : (sizeMB <= 8192 ? 8 : 16);

// The FAT has to describe the clusters that remain after the FAT itself, so solve for it.
let fatSize = 1;
for (let i = 0; i < 64; i++) {
    const dataSectors = totalSectors - RESERVED_SECTORS - NUM_FATS * fatSize;
    const clusters = Math.floor(dataSectors / secPerClus);
    const needed = Math.ceil(((clusters + 2) * 4) / BYTES_PER_SECTOR);
    if (needed === fatSize) break;
    fatSize = needed;
}

const dataStart = RESERVED_SECTORS + NUM_FATS * fatSize;
const clusterCount = Math.floor((totalSectors - dataStart) / secPerClus);
if (clusterCount < 65525) {
    console.error(`FAT32 needs at least 65525 clusters, this layout has ${clusterCount}`);
    process.exit(1);
}

// ---- boot sector ----
const boot = Buffer.alloc(BYTES_PER_SECTOR);
boot[0] = 0xeb; boot[1] = 0x58; boot[2] = 0x90;          // jump, as a real formatter writes
boot.write("MSWIN4.1", 3, "latin1");
boot.writeUInt16LE(BYTES_PER_SECTOR, 11);
boot[13] = secPerClus;
boot.writeUInt16LE(RESERVED_SECTORS, 14);
boot[16] = NUM_FATS;
boot.writeUInt16LE(0, 17);        // root entry count: zero on FAT32
boot.writeUInt16LE(0, 19);        // total sectors 16-bit: unused
boot[21] = 0xf8;                  // media descriptor: fixed disk
boot.writeUInt16LE(0, 22);        // FAT size 16-bit: unused on FAT32
boot.writeUInt16LE(63, 24);       // sectors per track
boot.writeUInt16LE(255, 26);      // heads
boot.writeUInt32LE(0, 28);        // hidden sectors: none, this is a superfloppy
boot.writeUInt32LE(totalSectors, 32);
boot.writeUInt32LE(fatSize, 36);
boot.writeUInt16LE(0, 40);        // flags: FATs are mirrored
boot.writeUInt16LE(0, 42);        // version
boot.writeUInt32LE(2, 44);        // root directory cluster
boot.writeUInt16LE(1, 48);        // FSInfo sector
boot.writeUInt16LE(6, 50);        // backup boot sector
boot[64] = 0x80;                  // drive number
boot[66] = 0x29;                  // extended boot signature
boot.writeUInt32LE(0x57494954, 67); // volume id
boot.write("WIITALE    ", 71, "latin1");
boot.write("FAT32   ", 82, "latin1");
boot.writeUInt16LE(0xaa55, 510);

// ---- FSInfo ----
const fsinfo = Buffer.alloc(BYTES_PER_SECTOR);
fsinfo.writeUInt32LE(0x41615252, 0);
fsinfo.writeUInt32LE(0x61417272, 484);
fsinfo.writeUInt32LE(clusterCount - 1, 488); // free clusters, root takes one
fsinfo.writeUInt32LE(3, 492);                // next free cluster hint
fsinfo.writeUInt16LE(0xaa55, 510);

// ---- first FAT sector: media descriptor, end-of-chain, and the root's own cluster ----
const fat0 = Buffer.alloc(BYTES_PER_SECTOR);
fat0.writeUInt32LE(0x0ffffff8, 0);
fat0.writeUInt32LE(0x0fffffff, 4);
fat0.writeUInt32LE(0x0ffffff8, 8); // cluster 2 = root directory, end of chain

console.log(`creating ${imagePath}`);
console.log(`  ${sizeMB} MB, ${totalSectors} sectors of ${BYTES_PER_SECTOR} B`);
console.log(`  ${secPerClus} sectors/cluster (${secPerClus * BYTES_PER_SECTOR} B), ` +
            `${clusterCount} clusters, FAT ${fatSize} sectors x${NUM_FATS}`);

const fd = fs.openSync(imagePath, "w");
try {
    // Lay the image out sparsely: only the metadata regions are written, and the file is
    // then extended to full length. Writing a gigabyte of zeroes would be pointless.
    fs.writeSync(fd, boot, 0, BYTES_PER_SECTOR, 0);
    fs.writeSync(fd, fsinfo, 0, BYTES_PER_SECTOR, 1 * BYTES_PER_SECTOR);
    fs.writeSync(fd, boot, 0, BYTES_PER_SECTOR, 6 * BYTES_PER_SECTOR);
    fs.writeSync(fd, fsinfo, 0, BYTES_PER_SECTOR, 7 * BYTES_PER_SECTOR);

    for (let i = 0; i < NUM_FATS; i++) {
        fs.writeSync(fd, fat0, 0, BYTES_PER_SECTOR,
                     (RESERVED_SECTORS + i * fatSize) * BYTES_PER_SECTOR);
    }

    // Zero the root directory cluster so it holds no stale entries.
    const rootCluster = Buffer.alloc(secPerClus * BYTES_PER_SECTOR);
    fs.writeSync(fd, rootCluster, 0, rootCluster.length, dataStart * BYTES_PER_SECTOR);

    fs.ftruncateSync(fd, totalSectors * BYTES_PER_SECTOR);
} finally {
    fs.closeSync(fd);
}

console.log("done");
