#ifndef _BS_WII_AUDIO_SYSTEM_H_
#define _BS_WII_AUDIO_SYSTEM_H_

#include "audio_system.h"

// ===[ Wii audio ]===
//
// Implements the runner's AudioSystem vtable on libogc's ASND. Undertale splits its
// audio in two, and so does this backend:
//
//  * Sound effects are PCM WAVs embedded in data.win's AUDO chunk (231 entries, 32.6 MB
//    of 44.1 kHz 16-bit audio). They are read straight out of data.win on demand into a
//    small MEM2 cache. ASND has little-endian voice formats, so the WAV payload is fed
//    to the DSP as-is with no conversion at all.
//
//  * Music is Ogg Vorbis in files next to the game, which GameMaker marks as streamed
//    rather than embedded. Those are decoded on the fly with stb_vorbis and pushed into
//    an ASND voice a block at a time; 90 MB of music could never be resident, and
//    decompressing it to PCM up front would be about a gigabyte.
//
// The hardware gives 16 voices, so at most 16 sounds are audible at once. The runner is
// allowed to ask for more; the quietest-priority request is the one that gets dropped.

// Matches the ids the shared runner code expects from every audio backend.
#define WII_MAX_SOUND_INSTANCES 64
#define WII_SOUND_INSTANCE_ID_BASE 100000
#define WII_MAX_AUDIO_STREAMS 32
#define WII_AUDIO_STREAM_INDEX_BASE 300000

// How much MEM2 the sound-effect cache may hold. Reserved from the MEM2 arena at init,
// so this must be created before the texture pool takes the rest.
#define WII_AUDIO_CACHE_BYTES (10u * 1024u * 1024u)

AudioSystem* WiiAudioSystem_create(const char* gameDir, const char* dataWinPath);

#endif /* _BS_WII_AUDIO_SYSTEM_H_ */
