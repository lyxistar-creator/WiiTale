# WiiTale

UNDERTALE, running natively on the Nintendo Wii.

It is the [Cinnamon](https://github.com/gemisis/butterscotch) GameMaker: Studio runner with
a backend written against libogc and the Hollywood's GX pipeline, built with devkitPPC. Not
an emulator: the game's own bytecode is interpreted and drawn through the console's own
graphics hardware.

**No game files are in this repository.** UNDERTALE is a commercial game; you supply your
own copy. This is the code that runs it.

The same backend also runs DELTARUNE Chapter 1, as
[DeltaWii](https://github.com/lyxistar-creator/DeltaWii).

## Status

Plays on a real Wii from a USB stick: the intro, the Ruins, dialogue, battles, sound
effects and streamed music all work, and saves survive across sessions. Held to a fixed 30
frames per second, which is what every one of the game's 336 rooms asks for.

Full detail, including everything that had to be worked out the hard way, is in
[cinnamon-main/WIITALE.md](cinnamon-main/WIITALE.md).

## Building

```
powershell -ExecutionPolicy Bypass -File cinnamon-main/build-wii.ps1
```

Needs devkitPPC. The wrapper exists because `powerpc-eabi-gcc.exe` reads the Windows `TMP`
variable, which Git Bash exports as a POSIX path that gcc cannot use, so it falls back to
`C:\Windows\` and fails.

## Preparing the assets

```
node cinnamon-main/tools/wiitale-preprocess/preprocess.mjs <data.win> textures.wtex
```

Texture pages cannot be used as they ship. Decoded to RGBA8 the 26 pages are 188 MB against
88 MB of total console RAM, and decoding a single 2048x2048 PNG needs about 37 MB of scratch
that does not exist there. Each page is converted offline: quantised to a 256-colour palette
and stored as CI8 where that is lossless, which most pixel art is, and kept as raw RGB5A3
where it would visibly band.

To check a pack against its source:

```
node cinnamon-main/tools/wiitale-preprocess/verify.mjs <data.win> textures.wtex
```

It re-reads the pack exactly the way `src/wii/wii_textures.c` does, untiles every page and
compares. On Undertale 1.08, 20 of 26 pages come back bit-exact.

## SD card layout

```
sd:/apps/wiitale/boot.dol
sd:/apps/wiitale/meta.xml
sd:/apps/wiitale/data.win        the game's own file
sd:/apps/wiitale/textures.wtex   generated above
sd:/apps/wiitale/*.ogg           the game's music
```

`usb:/apps/wiitale/` works too, and is searched after the SD card.

## Two hardware facts that shape everything

**GX cannot address a texture larger than 1024 texels in either axis.** GameMaker's atlas
pages are up to 2048x2048, so anything packed past row or column 1024 is simply unreachable
and samples nonsense. Every page is cut into tiles and a sprite spanning a seam is drawn in
pieces.

**There are 88 MB of RAM in total**, 24 of MEM1 and 64 of MEM2, and `data.win` alone is 60
MB. Nothing is loaded whole: TXTR and AUDO are indexed and streamed, rooms are parsed on
demand, and texture tiles live in a MEM2 pool with least-recently-used eviction.

## Licence

The Cinnamon runner keeps its own licence, in `cinnamon-main/LICENSE`. The Wii backend in
`cinnamon-main/src/wii/` and the tools in `cinnamon-main/tools/wiitale-preprocess/` are new
work in this repository.
