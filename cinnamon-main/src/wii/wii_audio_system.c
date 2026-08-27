#include "wii_audio_system.h"

#include "log.h"
#include "data_win.h"
#include "utils.h"
#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include <malloc.h>

#include <gccore.h>
#include <asndlib.h>
#include <ogc/cache.h>
#include <ogc/system.h>

// stb_vorbis is compiled as its own translation unit; this pulls in just its
// declarations.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

// ASND gives 16 voices. Anything beyond that cannot be heard no matter how many
// instances the runner asks for.
#define VOICE_COUNT 16

// Decoded music is handed to the DSP a block at a time. Two blocks of 8192 stereo
// frames is about 190 ms of audio at 44.1 kHz, comfortably more than one game frame,
// so a slow disk read or a long game step cannot starve the voice.
#define STREAM_FRAMES 8192
#define STREAM_BLOCK_BYTES (STREAM_FRAMES * 2 * (int) sizeof(int16_t))

// ===[ AUDO index ]===
typedef struct {
    uint32_t offset;   // absolute offset in data.win of the WAV payload
    uint32_t size;
} AudoEntry;

// ===[ Sound effect cache ]===
// Fixed-size slots carved out of MEM2. Every AUDO entry fits in one slot, so there is
// no fragmentation and no allocator: a slot is either free, or holds one sound.
typedef struct {
    int32_t  audoIndex;      // AUDO entry in this slot, -1 when free
    uint8_t* base;           // MEM2, 32-byte aligned
    uint8_t* pcm;            // samples, moved to base so the DSP gets 32-byte alignment
    uint32_t pcmSize;        // padded up to a multiple of 32, which is what ASND is given
    uint32_t realSize;       // sample bytes before padding, for computing the duration
    uint32_t frameBytes;     // bytes per frame (channels * 2)
    uint32_t sampleRate;
    uint16_t channels;
    int32_t  refCount;       // voices currently playing from this slot
    uint64_t lastUsed;
} SfxSlot;

typedef struct {
    bool     active;
    int32_t  instanceId;
    int32_t  soundIndex;     // SOND index, or a stream index
    int32_t  voice;          // ASND voice, -1 when it never got one
    int32_t  slot;           // sfx cache slot, -1 for streams
    bool     loop;
    bool     paused;

    float    gain;
    float    gainTarget;
    float    gainRate;       // units per second while fading, 0 when not fading
    float    pitch;

    // ---- streaming music ----
    bool         isStream;
    stb_vorbis*  vorbis;
    char*        streamPath;
    int16_t*     blocks[2];  // MEM2, double buffered
    int          nextBlock;
    bool         streamEnded;
    uint32_t     streamRate;
    int          streamChannels;
    float        streamLength;
} SoundInstance;

// An audio_create_stream handle.
typedef struct {
    bool  active;
    char* path;
} StreamEntry;

typedef struct {
    AudioSystem base;

    char* gameDir;
    FILE* dataWinFp;

    AudoEntry* audo;
    uint32_t   audoCount;

    SfxSlot* slots;
    int32_t  slotCount;
    uint32_t slotBytes;

    SoundInstance instances[WII_MAX_SOUND_INSTANCES];
    StreamEntry   streams[WII_MAX_AUDIO_STREAMS];

    float    masterGain;
    uint64_t tick;
    bool     suspended;
} WiiAudioSystem;

// ===[ little-endian readers for data.win and RIFF ]===
static inline uint32_t rdU32LE(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
static inline uint16_t rdU16LE(const uint8_t* p) {
    return (uint16_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8));
}

// Walks data.win's FORM chunks and records where every AUDO payload lives, without
// reading any of them. Mirrors what the texture cache does for TXTR.
static bool indexAudo(WiiAudioSystem* wa, const char* dataWinPath) {
    wa->dataWinFp = fopen(dataWinPath, "rb");
    if (wa->dataWinFp == nullptr) {
        logError("WiiAudio: cannot open %s\n", dataWinPath);
        return false;
    }

    uint8_t hdr[8];
    if (fseek(wa->dataWinFp, 0, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, 8, wa->dataWinFp) != 8) return false;
    if (memcmp(hdr, "FORM", 4) != 0) {
        logWarn("WiiAudio: data.win has no FORM header\n");
    }

    long fileEnd;
    fseek(wa->dataWinFp, 0, SEEK_END);
    fileEnd = ftell(wa->dataWinFp);

    long p = 8;
    long audoAt = -1;
    while (p + 8 <= fileEnd) {
        uint8_t chunkHdr[8];
        fseek(wa->dataWinFp, p, SEEK_SET);
        if (fread(chunkHdr, 1, 8, wa->dataWinFp) != 8) break;
        uint32_t len = rdU32LE(chunkHdr + 4);
        if (memcmp(chunkHdr, "AUDO", 4) == 0) { audoAt = p + 8; break; }
        p += 8 + (long) len;
    }
    if (audoAt < 0) {
        logWarn("WiiAudio: no AUDO chunk, sound effects will be silent\n");
        return false;
    }

    uint8_t countBuf[4];
    fseek(wa->dataWinFp, audoAt, SEEK_SET);
    if (fread(countBuf, 1, 4, wa->dataWinFp) != 4) return false;
    wa->audoCount = rdU32LE(countBuf);
    if (wa->audoCount == 0 || wa->audoCount > 65535u) {
        logWarn("WiiAudio: implausible AUDO count %u\n", wa->audoCount);
        return false;
    }

    uint8_t* ptrs = (uint8_t*) malloc(wa->audoCount * 4u);
    if (ptrs == nullptr) return false;
    if (fread(ptrs, 1, wa->audoCount * 4u, wa->dataWinFp) != wa->audoCount * 4u) {
        free(ptrs);
        return false;
    }

    wa->audo = (AudoEntry*) calloc(wa->audoCount, sizeof(AudoEntry));
    if (wa->audo == nullptr) { free(ptrs); return false; }

    uint32_t largest = 0;
    for (uint32_t i = 0; i < wa->audoCount; i++) {
        uint32_t at = rdU32LE(ptrs + i * 4u);
        uint8_t lenBuf[4];
        fseek(wa->dataWinFp, (long) at, SEEK_SET);
        if (fread(lenBuf, 1, 4, wa->dataWinFp) != 4) continue;
        wa->audo[i].size = rdU32LE(lenBuf);
        wa->audo[i].offset = at + 4u;
        if (wa->audo[i].size > largest) largest = wa->audo[i].size;
    }
    free(ptrs);

    logInfo("WiiAudio: indexed %u AUDO entries, largest %u KB\n", wa->audoCount, largest / 1024u);
    return true;
}

// ===[ MEM2 slot pool ]===
static void initSlots(WiiAudioSystem* wa, uint32_t budget) {
    uint32_t largest = 0;
    for (uint32_t i = 0; i < wa->audoCount; i++) {
        if (wa->audo[i].size > largest) largest = wa->audo[i].size;
    }
    if (largest == 0) return;

    // Round the slot up so every entry fits and the slots stay 32-byte aligned.
    uint32_t slotBytes = (largest + 31u) & ~31u;

    uint32_t arenaLo = (uint32_t) SYS_GetArena2Lo();
    uint32_t arenaHi = (uint32_t) SYS_GetArena2Hi();
    uint32_t available = (arenaHi > arenaLo) ? (arenaHi - arenaLo) : 0u;
    if (budget > available) budget = available;

    int32_t count = (int32_t) (budget / slotBytes);
    if (count <= 0) {
        logWarn("WiiAudio: not enough MEM2 for even one sound slot (%u KB needed)\n", slotBytes / 1024u);
        return;
    }
    if (count > 32) count = 32;

    uint8_t* base = (uint8_t*) arenaLo;
    uint32_t misalign = ((uint32_t) base) & 31u;
    if (misalign != 0) base += (32u - misalign);
    SYS_SetArena2Lo((void*) (base + (uint32_t) count * slotBytes));

    wa->slots = (SfxSlot*) calloc((size_t) count, sizeof(SfxSlot));
    if (wa->slots == nullptr) return;

    for (int32_t i = 0; i < count; i++) {
        wa->slots[i].audoIndex = -1;
        wa->slots[i].base = base + (uint32_t) i * slotBytes;
    }
    wa->slotCount = count;
    wa->slotBytes = slotBytes;

    logInfo("WiiAudio: %d sound slots of %u KB in MEM2 (%u KB total)\n",
            count, slotBytes / 1024u, ((uint32_t) count * slotBytes) / 1024u);
}

// Parses the RIFF headers of a slot's payload and points pcm/pcmSize at the samples.
static bool parseWav(SfxSlot* slot, uint32_t totalSize) {
    const uint8_t* d = slot->base;
    if (totalSize < 44 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0)
        return false;

    uint32_t p = 12;
    bool haveFmt = false;
    while (p + 8 <= totalSize) {
        uint32_t id = rdU32LE(d + p);
        uint32_t sz = rdU32LE(d + p + 4);

        if (memcmp(d + p, "fmt ", 4) == 0 && p + 8 + 16 <= totalSize) {
            uint16_t tag = rdU16LE(d + p + 8);
            slot->channels = rdU16LE(d + p + 10);
            slot->sampleRate = rdU32LE(d + p + 12);
            uint16_t bits = rdU16LE(d + p + 22);
            // Only plain 16-bit PCM is supported. Everything Undertale embeds is
            // exactly that, and guessing at anything else would just produce noise.
            if (tag != 1 || bits != 16) return false;
            haveFmt = true;
        } else if (memcmp(d + p, "data", 4) == 0) {
            if (!haveFmt) return false;
            uint32_t avail = totalSize - (p + 8);
            uint32_t size = (sz < avail) ? sz : avail;
            if (size == 0) return false;

            // ASND requires the sample buffer to be "aligned and padded to 32 bytes".
            // The samples sit after the RIFF headers, at offset 44 in a typical WAV,
            // which is neither -- handing that pointer to the DSP produces garbled
            // audio. Sliding the samples down to the slot base fixes the alignment,
            // since the base itself is 32-byte aligned, and the header bytes we drop
            // leave room for the padding.
            memmove(slot->base, slot->base + p + 8, size);
            uint32_t padded = (size + 31u) & ~31u;
            memset(slot->base + size, 0, padded - size);

            slot->pcm = slot->base;
            slot->pcmSize = padded;
            slot->frameBytes = 2u * (slot->channels ? slot->channels : 1u);
            slot->realSize = size;
            return true;
        }
        (void) id;
        p += 8 + sz + (sz & 1u); // RIFF chunks are word aligned
    }
    return false;
}

// Finds the slot holding this AUDO entry, loading it (and evicting an idle slot) if
// necessary. Returns -1 when the sound cannot be made resident.
static int32_t acquireSlot(WiiAudioSystem* wa, int32_t audoIndex) {
    if (wa->slotCount == 0 || audoIndex < 0 || (uint32_t) audoIndex >= wa->audoCount) return -1;

    for (int32_t i = 0; i < wa->slotCount; i++) {
        if (wa->slots[i].audoIndex == audoIndex) {
            wa->slots[i].lastUsed = wa->tick;
            return i;
        }
    }

    int32_t victim = -1;
    uint64_t oldest = (uint64_t) -1;
    for (int32_t i = 0; i < wa->slotCount; i++) {
        if (wa->slots[i].audoIndex == -1) { victim = i; break; }
        // A slot that a voice is still reading from must not be reused underneath it.
        if (wa->slots[i].refCount > 0) continue;
        if (wa->slots[i].lastUsed < oldest) { oldest = wa->slots[i].lastUsed; victim = i; }
    }
    if (victim < 0) return -1;

    SfxSlot* slot = &wa->slots[victim];
    uint32_t size = wa->audo[audoIndex].size;
    if (size > wa->slotBytes) return -1;

    if (fseek(wa->dataWinFp, (long) wa->audo[audoIndex].offset, SEEK_SET) != 0) return -1;
    if (fread(slot->base, 1, size, wa->dataWinFp) != size) return -1;

    slot->audoIndex = audoIndex;
    slot->pcm = nullptr;
    slot->pcmSize = 0;
    slot->refCount = 0;
    slot->lastUsed = wa->tick;

    if (!parseWav(slot, size)) {
        logWarn("WiiAudio: AUDO entry %d is not 16-bit PCM WAV, skipping\n", audoIndex);
        slot->audoIndex = -1;
        return -1;
    }

    // The DSP reads this memory directly, so it has to leave the CPU's cache first.
    DCFlushRange(slot->base, size);
    return victim;
}

// ===[ instances ]===
static SoundInstance* findFreeInstance(WiiAudioSystem* wa) {
    for (int32_t i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (!wa->instances[i].active) return &wa->instances[i];
    }
    return nullptr;
}

static SoundInstance* findInstance(WiiAudioSystem* wa, int32_t id) {
    int32_t idx = id - WII_SOUND_INSTANCE_ID_BASE;
    if (idx < 0 || idx >= WII_MAX_SOUND_INSTANCES) return nullptr;
    SoundInstance* inst = &wa->instances[idx];
    return (inst->active && inst->instanceId == id) ? inst : nullptr;
}

static void releaseInstance(WiiAudioSystem* wa, SoundInstance* inst) {
    if (inst->voice >= 0) {
        ASND_StopVoice(inst->voice);
        inst->voice = -1;
    }
    if (inst->slot >= 0 && inst->slot < wa->slotCount) {
        if (wa->slots[inst->slot].refCount > 0) wa->slots[inst->slot].refCount--;
        inst->slot = -1;
    }
    if (inst->vorbis != nullptr) {
        stb_vorbis_close(inst->vorbis);
        inst->vorbis = nullptr;
    }
    for (int b = 0; b < 2; b++) {
        free(inst->blocks[b]);
        inst->blocks[b] = nullptr;
    }
    free(inst->streamPath);
    inst->streamPath = nullptr;
    inst->active = false;
}

// Applies an instance's gain to its voice. ASND takes 0..255 per side.
static void applyVoiceVolume(WiiAudioSystem* wa, SoundInstance* inst) {
    if (inst->voice < 0) return;
    float g = inst->gain * wa->masterGain;
    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    int v = (int) (g * 255.0f + 0.5f);
    ASND_ChangeVolumeVoice(inst->voice, v, v);
}

// ===[ streaming ]===
static bool startStream(WiiAudioSystem* wa, SoundInstance* inst, const char* relPath) {
    size_t len = strlen(wa->gameDir) + strlen(relPath) + 1;
    char* full = (char*) malloc(len);
    if (full == nullptr) return false;
    strcpy(full, wa->gameDir);
    strcat(full, relPath);

    int err = 0;
    inst->vorbis = stb_vorbis_open_filename(full, &err, nullptr);
    if (inst->vorbis == nullptr) {
        logWarn("WiiAudio: cannot open stream '%s' (stb_vorbis error %d)\n", full, err);
        free(full);
        return false;
    }
    inst->streamPath = full;

    stb_vorbis_info info = stb_vorbis_get_info(inst->vorbis);
    inst->streamRate = info.sample_rate;
    inst->streamChannels = info.channels > 1 ? 2 : 1;
    inst->streamLength = stb_vorbis_stream_length_in_seconds(inst->vorbis);
    inst->streamEnded = false;
    inst->nextBlock = 0;

    for (int b = 0; b < 2; b++) {
        inst->blocks[b] = (int16_t*) memalign(32, STREAM_BLOCK_BYTES);
        if (inst->blocks[b] == nullptr) {
            logWarn("WiiAudio: out of memory for stream buffers\n");
            return false;
        }
        memset(inst->blocks[b], 0, STREAM_BLOCK_BYTES);
    }
    return true;
}

// Handing ASND a callback is what keeps a streaming voice alive: with a null callback the
// voice stops as soon as its buffer runs out, which silenced every track after the first
// block. It stays empty on purpose -- decoding Vorbis and touching the SD card inside an
// audio interrupt would be a bad idea, so update() does the refilling.
static void streamVoiceCallback(int32_t voice) {
    (void) voice;
}

// Decodes one block. Returns the number of bytes produced, 0 at end of stream.
static int decodeBlock(SoundInstance* inst, int16_t* dst) {
    int frames = stb_vorbis_get_samples_short_interleaved(
        inst->vorbis, inst->streamChannels, dst, STREAM_FRAMES * inst->streamChannels);

    if (frames <= 0) {
        if (inst->loop) {
            stb_vorbis_seek(inst->vorbis, 0);
            frames = stb_vorbis_get_samples_short_interleaved(
                inst->vorbis, inst->streamChannels, dst, STREAM_FRAMES * inst->streamChannels);
        }
        if (frames <= 0) { inst->streamEnded = true; return 0; }
    }

    int bytes = frames * inst->streamChannels * (int) sizeof(int16_t);
    // Same 32-byte rule as the effect slots: the block buffer is already aligned, but a
    // partial decode can end on any boundary, so the tail is zero-filled up to 32.
    int padded = (bytes + 31) & ~31;
    if (padded > STREAM_BLOCK_BYTES) padded = bytes & ~31;
    if (padded > bytes) memset((uint8_t*) dst + bytes, 0, (size_t) (padded - bytes));

    DCFlushRange(dst, (uint32_t) padded);
    return padded;
}

// ===[ vtable ]===

static void wiiAudioInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    wa->base.dw = dataWin;
    (void) fileSystem;
}

static void wiiAudioDestroy(AudioSystem* audio) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    for (int32_t i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (wa->instances[i].active) releaseInstance(wa, &wa->instances[i]);
    }
    for (int32_t i = 0; i < WII_MAX_AUDIO_STREAMS; i++) free(wa->streams[i].path);

    ASND_End();
    if (wa->dataWinFp != nullptr) fclose(wa->dataWinFp);
    free(wa->audo);
    free(wa->slots);
    free(wa->gameDir);
    free(wa);
}

static void wiiAudioUpdate(AudioSystem* audio, float deltaTime) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    wa->tick++;

    for (int32_t i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &wa->instances[i];
        if (!inst->active) continue;

        // ---- gain fades ----
        if (inst->gainRate != 0.0f) {
            float step = inst->gainRate * deltaTime;
            if ((step > 0.0f && inst->gain + step >= inst->gainTarget) ||
                (step < 0.0f && inst->gain + step <= inst->gainTarget)) {
                inst->gain = inst->gainTarget;
                inst->gainRate = 0.0f;
            } else {
                inst->gain += step;
            }
            applyVoiceVolume(wa, inst);
        }

        // ---- keep streaming voices fed ----
        // ASND_TestVoiceBufferReady returns 1 when the second buffer is free and 0 when
        // it is not. SND_OK is 0, so comparing against it tested for exactly the wrong
        // condition and the voice was never refilled.
        if (inst->isStream && inst->voice >= 0 && !inst->paused && !inst->streamEnded) {
            while (ASND_TestVoiceBufferReady(inst->voice) == 1) {
                int16_t* dst = inst->blocks[inst->nextBlock];
                int bytes = decodeBlock(inst, dst);
                if (bytes <= 0) break;
                if (ASND_AddVoice(inst->voice, dst, bytes) != SND_OK) break;
                inst->nextBlock ^= 1;
            }
        }

        // ---- reap finished voices ----
        if (inst->voice >= 0 && !inst->paused) {
            int status = ASND_StatusVoice(inst->voice);

            // A streaming voice sits in SND_WAITING between blocks, which is healthy.
            // It is only finished once the decoder has run out and the DSP has drained.
            bool finished = (status == SND_UNUSED) &&
                            (!inst->isStream || inst->streamEnded);
            if (finished) releaseInstance(wa, inst);
        }
    }
}

static int32_t wiiAudioPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    (void) priority;

    const char* streamFile = nullptr;
    int32_t audoIndex = -1;
    float initialGain = 1.0f, initialPitch = 1.0f;

    if (soundIndex >= WII_AUDIO_STREAM_INDEX_BASE) {
        int32_t s = soundIndex - WII_AUDIO_STREAM_INDEX_BASE;
        if (s < 0 || s >= WII_MAX_AUDIO_STREAMS || !wa->streams[s].active) return -1;
        streamFile = wa->streams[s].path;
    } else {
        DataWin* dw = wa->base.dw;
        if (dw == nullptr || soundIndex < 0 || (uint32_t) soundIndex >= dw->sond.count) return -1;

        Sound* sound = &dw->sond.sounds[soundIndex];
        initialGain = sound->volume;
        initialPitch = sound->pitch;

        // GameMaker marks streamed sounds by leaving the embedded bit clear; those live
        // in an Ogg file beside the game rather than in AUDO.
        bool embedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
        if (embedded && sound->audioFile >= 0) {
            audoIndex = sound->audioFile;
        } else {
            streamFile = sound->file;
            if (streamFile == nullptr || streamFile[0] == '\0') return -1;
        }
    }

    // A track is never meant to play over itself. With more than one player, a trigger
    // that starts music fires once per player instance -- walking into Flowey with two
    // characters started his theme twice, half a second apart, which sounds like an echo.
    // Sound effects are left alone: overlapping copies of those are normal and wanted.
    if (streamFile != nullptr) {
        for (int32_t i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
            SoundInstance* other = &wa->instances[i];
            if (!other->active || !other->isStream) continue;
            if (other->soundIndex != soundIndex) continue;
            logInfo("WiiAudio: '%s' already playing, not starting a second copy\n", streamFile);
            return other->instanceId;
        }
    }

    SoundInstance* inst = findFreeInstance(wa);
    if (inst == nullptr) {
        logWarn("WiiAudio: no free sound instance for %d\n", soundIndex);
        return -1;
    }

    memset(inst, 0, sizeof(*inst));
    inst->voice = -1;
    inst->slot = -1;
    inst->loop = loop;
    inst->gain = initialGain;
    inst->gainTarget = initialGain;
    inst->pitch = initialPitch > 0.0f ? initialPitch : 1.0f;
    inst->soundIndex = soundIndex;

    int voice = ASND_GetFirstUnusedVoice();
    if (voice < 0) {
        logWarn("WiiAudio: all %d voices are busy, dropping sound %d\n", VOICE_COUNT, soundIndex);
        return -1;
    }

    if (streamFile != nullptr) {
        inst->isStream = true;
        if (!startStream(wa, inst, streamFile)) {
            releaseInstance(wa, inst);
            return -1;
        }
        inst->voice = voice;

        // stb_vorbis writes samples in the host's byte order, and this host is big-endian,
        // so a decoded stream is BE. The embedded WAV effects come off disk little-endian
        // and use the _LE formats instead. Declaring the wrong one does not fail, it just
        // swaps every sample's bytes, which comes out as loud static.
        int format = (inst->streamChannels == 2) ? VOICE_STEREO_16BIT_BE : VOICE_MONO_16BIT_BE;
        int bytes = decodeBlock(inst, inst->blocks[0]);
        if (bytes <= 0) {
            logWarn("WiiAudio: '%s' opened but decoded nothing\n", streamFile);
            releaseInstance(wa, inst);
            return -1;
        }
        inst->nextBlock = 1;

        int vol = (int) (inst->gain * wa->masterGain * 255.0f + 0.5f);
        int res = ASND_SetVoice(voice, format,
                                (int) ((float) inst->streamRate * inst->pitch), 0,
                                inst->blocks[0], bytes, vol, vol, streamVoiceCallback);

        logInfo("WiiAudio: stream '%s' %uHz %dch %.1fs -> voice %d vol %d (%d bytes, res %d)\n",
                streamFile, inst->streamRate, inst->streamChannels, inst->streamLength,
                voice, vol, bytes, res);
    } else {
        int32_t slot = acquireSlot(wa, audoIndex);
        if (slot < 0) {
            releaseInstance(wa, inst);
            return -1;
        }
        SfxSlot* s = &wa->slots[slot];
        s->refCount++;
        inst->slot = slot;
        inst->voice = voice;

        int format = (s->channels == 2) ? VOICE_STEREO_16BIT_LE : VOICE_MONO_16BIT_LE;
        int vol = (int) (inst->gain * wa->masterGain * 255.0f + 0.5f);
        // ASND has no loop flag; a looping effect is re-armed by SetInfiniteVoice.
        if (loop) {
            ASND_SetInfiniteVoice(voice, format, (int) ((float) s->sampleRate * inst->pitch),
                                  0, s->pcm, (int) s->pcmSize, vol, vol);
        } else {
            ASND_SetVoice(voice, format, (int) ((float) s->sampleRate * inst->pitch), 0,
                          s->pcm, (int) s->pcmSize, vol, vol, nullptr);
        }
    }

    inst->active = true;
    inst->instanceId = WII_SOUND_INSTANCE_ID_BASE + (int32_t) (inst - wa->instances);
    return inst->instanceId;
}

// Runs `fn` over the instance(s) the caller named: one instance, or every instance
// currently playing a given SOND index.
#define FOR_TARGET(wa, soundOrInstance, inst, body)                                   \
    do {                                                                              \
        if ((soundOrInstance) >= WII_SOUND_INSTANCE_ID_BASE &&                         \
            (soundOrInstance) < WII_AUDIO_STREAM_INDEX_BASE) {                         \
            SoundInstance* inst = findInstance((wa), (soundOrInstance));               \
            if (inst != nullptr) { body }                                              \
        } else {                                                                       \
            for (int32_t _i = 0; _i < WII_MAX_SOUND_INSTANCES; _i++) {                 \
                SoundInstance* inst = &(wa)->instances[_i];                            \
                if (!inst->active || inst->soundIndex != (soundOrInstance)) continue;  \
                body                                                                   \
            }                                                                          \
        }                                                                              \
    } while (0)

static void wiiAudioStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    FOR_TARGET(wa, soundOrInstance, inst, { releaseInstance(wa, inst); });
}

static void wiiAudioStopAll(AudioSystem* audio) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    for (int32_t i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (wa->instances[i].active) releaseInstance(wa, &wa->instances[i]);
    }
}

static bool wiiAudioIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    bool playing = false;
    FOR_TARGET(wa, soundOrInstance, inst, { if (!inst->paused) playing = true; });
    return playing;
}

static void wiiAudioPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    FOR_TARGET(wa, soundOrInstance, inst, {
        if (inst->voice >= 0) ASND_PauseVoice(inst->voice, 1);
        inst->paused = true;
    });
}

static void wiiAudioResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    FOR_TARGET(wa, soundOrInstance, inst, {
        if (inst->voice >= 0) ASND_PauseVoice(inst->voice, 0);
        inst->paused = false;
    });
}

static void wiiAudioPauseAll(AudioSystem* audio) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    for (int32_t i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &wa->instances[i];
        if (!inst->active) continue;
        if (inst->voice >= 0) ASND_PauseVoice(inst->voice, 1);
        inst->paused = true;
    }
}

static void wiiAudioResumeAll(AudioSystem* audio) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    for (int32_t i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &wa->instances[i];
        if (!inst->active) continue;
        if (inst->voice >= 0) ASND_PauseVoice(inst->voice, 0);
        inst->paused = false;
    }
}

static void wiiAudioSuspend(AudioSystem* audio) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    wa->suspended = true;
    ASND_Pause(1);
}

static void wiiAudioResume(AudioSystem* audio) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    wa->suspended = false;
    ASND_Pause(0);
}

static void wiiAudioSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    FOR_TARGET(wa, soundOrInstance, inst, {
        inst->gainTarget = gain;
        if (timeMs == 0) {
            inst->gain = gain;
            inst->gainRate = 0.0f;
            applyVoiceVolume(wa, inst);
        } else {
            inst->gainRate = (gain - inst->gain) / ((float) timeMs / 1000.0f);
        }
    });
}

static float wiiAudioGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    float g = 0.0f;
    FOR_TARGET(wa, soundOrInstance, inst, { g = inst->gain; });
    return g;
}

static void wiiAudioSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    if (pitch <= 0.0f) return;
    FOR_TARGET(wa, soundOrInstance, inst, {
        inst->pitch = pitch;
        if (inst->voice >= 0) {
            uint32_t rate = inst->isStream ? inst->streamRate
                          : (inst->slot >= 0 ? wa->slots[inst->slot].sampleRate : 44100u);
            ASND_ChangePitchVoice(inst->voice, (int) ((float) rate * pitch));
        }
    });
}

static float wiiAudioGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    float p = 1.0f;
    FOR_TARGET(wa, soundOrInstance, inst, { p = inst->pitch; });
    return p;
}

static float wiiAudioGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    float pos = 0.0f;
    FOR_TARGET(wa, soundOrInstance, inst, {
        if (inst->voice >= 0) pos = (float) ASND_GetTimerVoice(inst->voice) / 1000.0f;
    });
    return pos;
}

static void wiiAudioSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    FOR_TARGET(wa, soundOrInstance, inst, {
        // Seeking is only meaningful for a decoded stream. ASND gives no way to restart
        // a PCM voice partway through its buffer, so seeking an effect is ignored
        // rather than silently restarting it from the beginning.
        if (inst->isStream && inst->vorbis != nullptr) {
            unsigned int frame = (unsigned int) (positionSeconds * (float) inst->streamRate);
            stb_vorbis_seek(inst->vorbis, frame);
            inst->streamEnded = false;
        }
    });
}

static float wiiAudioGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    float len = 0.0f;
    FOR_TARGET(wa, soundOrInstance, inst, {
        if (inst->isStream) {
            len = inst->streamLength;
        } else if (inst->slot >= 0) {
            SfxSlot* s = &wa->slots[inst->slot];
            // realSize, not pcmSize: the padding is silence and must not count.
            if (s->sampleRate > 0 && s->frameBytes > 0)
                len = (float) (s->realSize / s->frameBytes) / (float) s->sampleRate;
        }
    });
    return len;
}

static void wiiAudioSetMasterGain(AudioSystem* audio, float gain) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    wa->masterGain = gain;
    for (int32_t i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (wa->instances[i].active) applyVoiceVolume(wa, &wa->instances[i]);
    }
}

static void wiiAudioSetMasterGainForListener(AudioSystem* audio, float gain, int32_t listenerId) {
    // Undertale is 2D and single-listener; there is nothing per-listener to apply.
    (void) listenerId;
    wiiAudioSetMasterGain(audio, gain);
}

static void wiiAudioSetChannelCount(AudioSystem* audio, int32_t count) {
    // The DSP's voice count is fixed at 16; the runner's request is advisory.
    (void) audio; (void) count;
}

static void wiiAudioGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    // Undertale ships a single audio group, and AUDO is streamed per sound anyway, so
    // there is nothing to bring in ahead of time.
    (void) audio; (void) groupIndex;
}

static bool wiiAudioGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    (void) audio; (void) groupIndex;
    return true;
}

static int32_t wiiAudioCreateStream(AudioSystem* audio, const char* filename) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    if (filename == nullptr) return -1;
    for (int32_t i = 0; i < WII_MAX_AUDIO_STREAMS; i++) {
        if (wa->streams[i].active) continue;
        wa->streams[i].path = safeStrdup(filename);
        wa->streams[i].active = true;
        return WII_AUDIO_STREAM_INDEX_BASE + i;
    }
    logWarn("WiiAudio: out of stream handles\n");
    return -1;
}

static bool wiiAudioDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    WiiAudioSystem* wa = (WiiAudioSystem*) audio;
    int32_t s = streamIndex - WII_AUDIO_STREAM_INDEX_BASE;
    if (s < 0 || s >= WII_MAX_AUDIO_STREAMS || !wa->streams[s].active) return false;
    free(wa->streams[s].path);
    wa->streams[s].path = nullptr;
    wa->streams[s].active = false;
    return true;
}

static AudioSystemVtable gWiiAudioVtable = {
    .init = wiiAudioInit,
    .destroy = wiiAudioDestroy,
    .update = wiiAudioUpdate,
    .playSound = wiiAudioPlaySound,
    .stopSound = wiiAudioStopSound,
    .stopAll = wiiAudioStopAll,
    .isPlaying = wiiAudioIsPlaying,
    .pauseSound = wiiAudioPauseSound,
    .resumeSound = wiiAudioResumeSound,
    .pauseAll = wiiAudioPauseAll,
    .resumeAll = wiiAudioResumeAll,
    .suspend = wiiAudioSuspend,
    .resume = wiiAudioResume,
    .setSoundGain = wiiAudioSetSoundGain,
    .getSoundGain = wiiAudioGetSoundGain,
    .setSoundPitch = wiiAudioSetSoundPitch,
    .getSoundPitch = wiiAudioGetSoundPitch,
    .getTrackPosition = wiiAudioGetTrackPosition,
    .setTrackPosition = wiiAudioSetTrackPosition,
    .getSoundLength = wiiAudioGetSoundLength,
    .setMasterGain = wiiAudioSetMasterGain,
    .setMasterGainForListener = wiiAudioSetMasterGainForListener,
    .setChannelCount = wiiAudioSetChannelCount,
    .groupLoad = wiiAudioGroupLoad,
    .groupIsLoaded = wiiAudioGroupIsLoaded,
    .createStream = wiiAudioCreateStream,
    .destroyStream = wiiAudioDestroyStream,
};

AudioSystem* WiiAudioSystem_create(const char* gameDir, const char* dataWinPath) {
    WiiAudioSystem* wa = (WiiAudioSystem*) calloc(1, sizeof(WiiAudioSystem));
    if (wa == nullptr) return nullptr;

    wa->base.vtable = &gWiiAudioVtable;
    wa->masterGain = 1.0f;
    wa->gameDir = safeStrdup(gameDir);
    for (int32_t i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        wa->instances[i].voice = -1;
        wa->instances[i].slot = -1;
    }

    ASND_Init();
    ASND_Pause(0);

    if (indexAudo(wa, dataWinPath)) {
        initSlots(wa, WII_AUDIO_CACHE_BYTES);
    }

    logInfo("WiiAudio: ready (%d voices, %d cache slots)\n", VOICE_COUNT, wa->slotCount);
    return (AudioSystem*) wa;
}
