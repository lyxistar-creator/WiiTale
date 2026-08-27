#!/usr/bin/env node
// Cross-references data.win: which GML routines touch a given function or variable, and
// which functions and variables a given routine touches.
//
//   node whocalls.mjs <data.win> --uses <function-or-variable> [...]
//   node whocalls.mjs <data.win> --inside <routine-name> [...]
//
// GameMaker stores, for every name, the address of its first reference in the bytecode;
// each reference then carries the byte offset to the next one, in the word that follows
// the instruction, masked to 24 bits. Walking those chains and matching addresses against
// the CODE chunk's routines gives a cross-reference without a decompiler.
//
// The masking was found by trying the plausible encodings and keeping the one that walked
// every chain to its declared length, rather than by guessing.

import fs from "node:fs";

const [path, ...rest] = process.argv.slice(2);
if (!path || rest.length === 0) {
    console.error("usage: node whocalls.mjs <data.win> --uses <name>... | --inside <routine>...");
    process.exit(2);
}

const b = fs.readFileSync(path);

function chunkOf(name) {
    let p = 8;
    while (p + 8 <= b.length) {
        const n = b.toString("latin1", p, p + 4);
        const len = b.readUInt32LE(p + 4);
        if (n === name) return { start: p + 8, len };
        p += 8 + len;
    }
    return null;
}

function str(ptr) {
    if (!ptr) return "";
    const len = b.readUInt32LE(ptr - 4);
    return b.toString("latin1", ptr, ptr + len);
}

// ---- routines and the file range each one's bytecode occupies ----
const code = chunkOf("CODE");
const codeLo = code.start, codeHi = code.start + code.len;
const codeCount = b.readUInt32LE(code.start);
const routines = [];
for (let i = 0; i < codeCount; i++) {
    const ptr = b.readUInt32LE(code.start + 4 + i * 4);
    if (!ptr) continue;
    const relField = ptr + 12;           // after name, length, locals, args
    const rel = b.readInt32LE(relField);
    routines.push({
        name: str(b.readUInt32LE(ptr)),
        start: relField + rel,
        end: relField + rel + b.readUInt32LE(ptr + 4),
    });
}
routines.sort((x, y) => x.start - y.start);

function routineAt(addr) {
    let lo = 0, hi = routines.length - 1;
    while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        if (addr < routines[mid].start) hi = mid - 1;
        else if (addr >= routines[mid].end) lo = mid + 1;
        else return routines[mid].name;
    }
    return null;
}

// ---- names: functions and variables, each with its reference chain ----
const SCOPE = { "-1": "self", "-2": "other", "-3": "all", "-4": "noone", "-5": "global", "-6": "local", "-7": "builtin" };

const names = [];

const func = chunkOf("FUNC");
const funcCount = b.readUInt32LE(func.start);
for (let i = 0; i < funcCount; i++) {
    const o = func.start + 4 + i * 12;
    names.push({
        kind: "fn", scope: "",
        name: str(b.readUInt32LE(o)),
        occurrences: b.readUInt32LE(o + 4),
        firstAddress: b.readUInt32LE(o + 8),
    });
}

const vari = chunkOf("VARI");
const variCount = (vari.len - 12) / 20;
for (let i = 0; i < variCount; i++) {
    const o = vari.start + 12 + i * 20;
    names.push({
        kind: "var", scope: SCOPE[b.readInt32LE(o + 4)] ?? String(b.readInt32LE(o + 4)),
        name: str(b.readUInt32LE(o)),
        occurrences: b.readUInt32LE(o + 12),
        firstAddress: b.readUInt32LE(o + 16),
    });
}

// The chain: the word after the instruction holds the distance to the next reference in
// its low 24 bits; the top byte is a type tag.
function* chain(entry) {
    let addr = entry.firstAddress;
    for (let i = 0; i < entry.occurrences; i++) {
        if (addr < codeLo || addr + 8 > codeHi) return;
        yield addr;
        const step = b.readUInt32LE(addr + 4) & 0x00ffffff;
        if (step === 0) return;
        addr += step;
    }
}

const mode = rest[0];
const targets = rest.slice(1);

if (mode === "--uses") {
    for (const t of targets) {
        const matches = names.filter((n) => n.name === t);
        if (matches.length === 0) { console.log(`${t}: not found\n`); continue; }
        for (const entry of matches) {
            console.log(`${entry.name}  [${entry.kind}${entry.scope ? " " + entry.scope : ""}]  ${entry.occurrences} reference(s)`);
            const callers = new Map();
            for (const addr of chain(entry)) {
                const r = routineAt(addr) || "(unmapped)";
                callers.set(r, (callers.get(r) || 0) + 1);
            }
            for (const [r, n] of [...callers].sort((a, c) => c[1] - a[1]).slice(0, 25)) {
                console.log(`   ${String(n).padStart(4)} x  ${r}`);
            }
            console.log();
        }
    }
} else if (mode === "--inside") {
    // Build address -> name once, then report what falls inside each requested routine.
    const wanted = routines.filter((r) => targets.some((t) => r.name === t || r.name.includes(t)));
    if (wanted.length === 0) { console.log("no routine matched"); process.exit(1); }

    const found = new Map(wanted.map((r) => [r.name, []]));
    for (const entry of names) {
        if (entry.occurrences === 0) continue;
        for (const addr of chain(entry)) {
            for (const r of wanted) {
                if (addr >= r.start && addr < r.end) {
                    found.get(r.name).push(entry);
                    break;
                }
            }
        }
    }

    for (const r of wanted) {
        const list = found.get(r.name);
        const seen = new Map();
        for (const e of list) {
            const key = `${e.kind}${e.scope ? " " + e.scope : ""}  ${e.name}`;
            seen.set(key, (seen.get(key) || 0) + 1);
        }
        console.log(`${r.name}  (${list.length} references)`);
        for (const [k, n] of [...seen].sort((a, c) => c[1] - a[1])) {
            console.log(`   ${String(n).padStart(3)} x  ${k}`);
        }
        console.log();
    }
} else {
    console.error(`unknown mode ${mode}`);
    process.exit(2);
}
