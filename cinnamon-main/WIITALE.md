# WiiTale

A native Nintendo Wii backend for the Cinnamon GameMaker: Studio runner, targeting
Undertale 1.08 (WAD/bytecode version 16).

## Building

```bash
powershell -ExecutionPolicy Bypass -File build-wii.ps1
```

Produces `wiitale.dol`. `build-wii.ps1 clean` removes the build tree.

The script exists because `powerpc-eabi-gcc.exe` is a native Windows binary and reads
Windows `TMP`. Git Bash exports that as a POSIX path, which gcc cannot use, so it falls
back to `C:\Windows\` and fails. The script sets the toolchain paths and a writable
Windows `TMP`, then runs `make -f Makefile.wii`.

## Preparing the assets

Texture pages cannot be used as they ship. Decoded to RGBA8 the 26 pages are 188 MB,
against 88 MB of total Wii RAM, and decoding a single 2048x2048 PNG needs about 37 MB of
scratch that does not exist on the console. The preprocessor converts them offline:

```bash
node tools/wiitale-preprocess/preprocess.mjs <path-to-data.win> textures.wtex
```

It needs nothing but Node — PNG decoding uses Node's own zlib.

Each page is converted to the format that suits it. Pages that fit inside 256 colours
become CI8 (one byte per texel plus a palette), which is lossless for pixel art; pages
whose palettised form exceeds an RMSE of 8 are kept as raw RGB5A3 instead, because the
banding would otherwise be visible. For Undertale that comes out as 21 CI8 pages and 5
RGB5A3 pages, 61 MB in total, with a largest page of 8 MB.

To check a generated pack against its source:

```bash
node tools/wiitale-preprocess/verify.mjs <path-to-data.win> textures.wtex
```

This re-reads the pack exactly the way `src/wii/wii_textures.c` does, untiles every page
and compares it to the original image. On Undertale 1.08, 20 of 26 pages come back
bit-exact and the worst page has an RMSE of 4.5.

## SD card layout

```
sd:/apps/wiitale/boot.dol       from dist/apps/wiitale/
sd:/apps/wiitale/meta.xml       from dist/apps/wiitale/
sd:/apps/wiitale/data.win       the game's own file
sd:/apps/wiitale/textures.wtex  generated above
sd:/apps/wiitale/*.ogg          the game's external music files
```

The runner also accepts `sd:/wiitale/`, `usb:/apps/wiitale/` and `usb:/wiitale/`.

## Controls

Held sideways, NES-style, which is the default:

| Wiimote | Action |
| --- | --- |
| D-pad | Move (rotated a quarter turn for the sideways grip) |
| 2 | Confirm (Z) |
| 1 | Cancel (X) |
| Minus | Menu (C) |
| Plus | Enter |
| HOME | Open the menu |

Nunchuk and Classic Controller are also handled; with a Nunchuk attached the d-pad is
read upright, since that is the only way the remote can be held.

On-screen prompts are rewritten to name Wii buttons, at draw time rather than in
`data.win`, so the game file is never modified. The table lives next to the input
mapping in `src/wii/wii_renderer.c` and has to be kept in step with it.

### The menu

HOME opens it. Everything this port adds lives there, because the features used to sit
behind button combinations and nobody found them: the save slots went unnoticed for a
whole session because they were on Minus + 1.

| Entry | What it does |
| --- | --- |
| RESUME | back to the game |
| SAVE SLOTS | three slots plus an automatic backup |
| ROOM WARP | jump to any room, including unreachable ones |
| QUIT | back to the Homebrew Channel |

Along the bottom is the measured frame rate next to the target, and a count of frames that
overran. "Locked to 30" is a claim about the console keeping up, and this is how it can be
checked rather than taken on trust.

Both sub-menus are driven either by aiming or by the d-pad. Pointing takes over the moment
the remote is actually moved, not merely aimed somewhere: a remote resting on the sofa
still points at the screen, and it would otherwise take the selection back the instant the
d-pad was used. Losing the sensor bar leaves everything on the d-pad.

Overlay text is drawn at twice the game's own size. These fonts are eight pixels tall,
meant for a monitor an arm's length away, and on a television they were reported as almost
unreadable.

| Combination | What it does |
| --- | --- |
| Minus + Plus | Toggle pointer steering |
| Hold 1 + 2 | Show the texture cache inspector |

**Pointer steering** walks the player toward the cursor. It is not path-finding: there is
no collision map here, so the player walks straight at the target and leans on any wall in
between, exactly as if the d-pad were held. The player's real position is read from the
`obj_mainchara` instance and the cursor is mapped back through the view rectangle, so it
works in rooms that do not scroll. Losing sight of the sensor bar falls back to the d-pad
rather than stopping the player.

**The warp menu** lists all 336 rooms, including ones normally unreachable, two columns by
ten. It was three columns until the text was doubled in size, at which point the names
overlapped into a smear. The d-pad walks the grid and running off either side turns the
page and re-enters from the other, so the whole list is one continuous run; Plus and Minus
jump a page at a time. A warps, B closes. The game is not stepped while it is open, so
nothing runs on unattended behind it.

## How memory is kept inside 88 MB

The Wii has 24 MB of MEM1 and 64 MB of MEM2. Undertale's `data.win` is 60 MB on its own,
so nothing is loaded whole:

- `TXTR` (12 MB) and `AUDO` (33 MB) are never parsed. Both are indexed and streamed.
- Rooms are parsed on demand (`lazyLoadRooms`).
- Texture pages live in a 40 MB MEM2 pool with least-recently-used eviction. It was 24 MB,
  chosen before anyone measured: MEM2 actually had 43 MB free, and against a 62 MB pack the
  cache was evicting tiles it needed again the same frame and re-reading them off the card.
  A 2 MB tile fetched over USB several times a frame is not a frame rate problem, it is a
  disk problem wearing one as a disguise. GX has only 16 hardware TLUT slots against 26
  pages, so palette slots are tied to residency rather than to page identity, and that
  still caps how many pages can be resident.
- Sound effects live in a 10 MB MEM2 pool of fixed-size slots, read straight out of
  `data.win`. ASND has little-endian voice formats, so the WAV payload reaches the DSP
  with no conversion.
- Music is decoded from Ogg Vorbis a block at a time with stb_vorbis. 90 MB of music
  could not be resident, and decompressed it would be roughly a gigabyte.

## What is not implemented

- **Shaders.** The Hollywood has no programmable pipeline, only the TEV. The shader
  entry points report "unsupported" so GML that guards on `shaders_are_supported()`
  takes the correct branch.
- **`surface_getpixel` / `sprite_create_from_surface`.** Both need a CPU readback of the
  EFB that is not implemented; they report failure rather than returning wrong data.
- **`gpu_set_blendmode_ext` alpha factors.** GX applies one factor pair to colour and
  alpha together. The alpha factors are stored so the getter round-trips, but only the
  colour pair reaches the hardware.
- **`bm_max` / `bm_min`.** GX has no max or min blend equation; both approximate.

## Status

Plays on a real Wii, from a USB stick: the intro, the Ruins, dialogue, battles, sound
effects and streamed music all work, and saves survive across sessions.

The loop is held to a fixed 30 frames per second, which is the rate every one of the
game's 336 rooms asks for. Fixed, not merely capped: the runner is told a constant amount
of time passed, so a room that is expensive to draw becomes slow motion rather than
speeding up to compensate. The menu shows what the console is actually managing against
that target.

Things worth knowing, all of them found the hard way:

- **GX cannot address a texture larger than 1024x1024.** Pages are split into tiles and
  sprites spanning a seam are drawn in pieces. Nothing packed below row 1024 of a tall
  page renders without this.
- **The loop must be paced, and paced with a fixed step.** The video interface returns at
  60 Hz and Undertale's rooms run at 30, so without pacing the whole game runs at double
  speed. Handing the runner the *measured* elapsed time instead is a subtler version of the
  same mistake: an expensive room then runs faster to compensate, so a scene's pace depends
  on how hard it happens to be to draw.
- **Decoded Vorbis is big-endian, embedded WAV is little-endian.** They need different
  ASND voice formats; using one for the other produces loud static, not silence.
- **`ASND_TestVoiceBufferReady` returns 1 when ready**, not `SND_OK` (which is 0), and a
  streaming voice needs a non-null callback or it stops after its first block.
- **`Runner.displayScaleX/Y` are never initialised by the shared code**, so the backend
  has to compute them or every view collapses to a zero-sized viewport.
- **The libogc console draws into a framebuffer.** Logging anything after the game starts
  drawing paints over the frame; the console is switched off once the main loop begins.
