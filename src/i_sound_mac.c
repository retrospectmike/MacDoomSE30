/*
 * i_sound_mac.c — Sound Manager SFX for Doom SE/30
 *
 * Fire-and-forget via SndPlay on the Apple Sound Chip (ASC).
 * No software mixing, no pitch shifting, no stereo — mono speaker.
 * Music stubs remain (no MUS→MIDI).
 *
 * Guarded by opt_sound (doom.cfg / -sound): when 0, all functions are no-ops.
 */

#include "doomdef.h"
#include "doomstat.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "sounds.h"
#include "m_swap.h"

#include <Sound.h>
#include <string.h>

extern int opt_sound;
extern int numChannels;  /* from s_sound.c, set by m_misc.c defaults */

/* ---- channel state ---- */

#define MAC_MAX_CHANNELS 4

typedef struct {
    SndChannelPtr   chan;       /* Sound Manager channel */
    Handle          snd_handle; /* current 'snd ' resource Handle (disposed on next play) */
    int             sfx_id;    /* DOOM sfx enum currently playing, or -1 */
    void           *cache_ptr; /* pointer to zone block (for unpin on completion) */
    volatile Boolean playing;  /* DOOM-level: cleared by ISStop or callback */
    volatile Boolean hw_busy;  /* HW-level: set on SndPlay, cleared ONLY by callback */
} mac_channel_t;

static mac_channel_t mac_channels[MAC_MAX_CHANNELS];
static int mac_num_channels = 0;
static SndCallbackUPP snd_callback_upp = NULL;

/* Throttle: max new sounds per frame to avoid SndDoImmediate pileup */
static int snd_starts_this_frame = 0;
#define SND_MAX_STARTS_PER_FRAME 2

/* ---- callback (runs at interrupt time) ---- */

static pascal void snd_callback(SndChannelPtr chan, SndCommand *cmd)
{
    int i;
    (void)cmd;
    for (i = 0; i < mac_num_channels; i++) {
        if (mac_channels[i].chan == chan) {
            mac_channels[i].playing = false;
            mac_channels[i].hw_busy = false;
            break;
        }
    }
}

/* ---- init / shutdown ---- */

void I_InitSound(void)
{
    int i;
    OSErr err;
    int want;

    if (!opt_sound) {
        doom_log("I_InitSound: sound disabled (opt_sound=0)\r");
        return;
    }

    snd_callback_upp = NewSndCallbackUPP(snd_callback);

    /* Cap channel count */
    want = numChannels;
    if (want < 1) want = 1;
    if (want > MAC_MAX_CHANNELS) want = MAC_MAX_CHANNELS;

    for (i = 0; i < want; i++) {
        mac_channels[i].chan = NULL;
        err = SndNewChannel(&mac_channels[i].chan, sampledSynth, 0x80 /* initMono */,
                            snd_callback_upp);
        if (err != noErr) {
            doom_log("I_InitSound: SndNewChannel[%d] err=%d\r", i, (int)err);
            break;
        }
        mac_channels[i].sfx_id = -1;
        mac_channels[i].playing = false;
        mac_channels[i].hw_busy = false;
        mac_channels[i].cache_ptr = NULL;
        mac_channels[i].snd_handle = NULL;
        mac_num_channels++;
    }

    doom_log("I_InitSound: %d channels allocated (wanted %d)\r",
             mac_num_channels, want);
}

void I_ShutdownSound(void)
{
    int i;

    for (i = 0; i < mac_num_channels; i++) {
        if (mac_channels[i].chan) {
            SndDisposeChannel(mac_channels[i].chan, true);
            mac_channels[i].chan = NULL;
        }
        if (mac_channels[i].snd_handle) {
            DisposeHandle(mac_channels[i].snd_handle);
            mac_channels[i].snd_handle = NULL;
        }
        if (mac_channels[i].cache_ptr) {
            Z_ChangeTag(mac_channels[i].cache_ptr, PU_CACHE);
            mac_channels[i].cache_ptr = NULL;
        }
    }
    mac_num_channels = 0;

    if (snd_callback_upp) {
        DisposeSndCallbackUPP(snd_callback_upp);
        snd_callback_upp = NULL;
    }
}

void I_SetChannels(void) { }

/* ---- SFX lump lookup ---- */

int I_GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfxinfo->name);
    return W_GetNumForName(namebuf);
}

/* ---- DMX lump → SoundHeader ---- */

/*
 * DMX sound lump format (little-endian in WAD):
 *   bytes 0-1: format tag (0x0003 = PCM)
 *   bytes 2-3: sample rate (e.g. 11025)
 *   bytes 4-7: sample count
 *   bytes 8+:  8-bit unsigned PCM data
 *
 * 8-bit unsigned is native Mac Sound Manager format — no conversion needed.
 * We read header fields byte-by-byte to handle little-endian without macros.
 */
static int I_CacheSfx(int sfx_id)
{
    sfxinfo_t *sfx = &S_sfx[sfx_id];
    byte *raw;
    unsigned short srate;
    unsigned long scount;

    if (sfx->lumpnum < 0)
        sfx->lumpnum = I_GetSfxLumpNum(sfx);

    /* Cache the raw WAD lump (PU_CACHE — auto-purgeable when not pinned) */
    raw = (byte *)W_CacheLumpNum(sfx->lumpnum, PU_CACHE);
    if (!raw) return 0;

    /* Parse little-endian DMX header manually */
    srate  = (unsigned short)(raw[2] | (raw[3] << 8));
    scount = (unsigned long)(raw[4] | (raw[5] << 8) |
                             (raw[6] << 16) | (raw[7] << 24));

    /* Sanity checks */
    if (srate == 0 || scount == 0 || scount > 200000) return 0;

    /* Store the raw pointer so we can pin/unpin later */
    sfx->data = raw;

    return 1;
}

/* ---- helper: build format 1 'snd ' resource Handle ---- */

/*
 * Builds a 44-byte format 1 'snd ' resource in a new Handle.
 * If samplePtr is NULL, data should be appended at offset 42 (inline).
 * If samplePtr is non-NULL, Sound Manager reads from that pointer.
 * Caller must DisposeHandle when done.
 */
static Handle I_BuildSndHandle(Ptr samplePtr, long length,
                                unsigned long sampleRate,
                                long extra_bytes)
{
    long sz = 52 + extra_bytes; /* 20 hdr + 2 cmds(16) + 24 SoundHeader + inline data */
    Handle h = NewHandle(sz);
    unsigned char *p;

    if (!h) return NULL;
    HLock(h);
    p = (unsigned char *)*h;
    memset(p, 0, sz);

    /* Format 1 'snd ' resource header (20 bytes) */
    *(short *)(p + 0) = 1;                /* format type */
    *(short *)(p + 2) = 1;                /* num modifiers */
    *(short *)(p + 4) = 5;                /* sampledSynth */
    *(long *)(p + 6) = 0x00000080;        /* initMono */
    *(short *)(p + 10) = 2;               /* num commands */
    *(unsigned short *)(p + 12) = 0x8051; /* bufferCmd | dataOffsetFlag */
    *(short *)(p + 14) = 0;               /* param1 */
    *(long *)(p + 16) = 28;               /* param2: offset to SoundHeader */
    /* callBackCmd fires when buffer drains → snd_callback sets playing=false */
    *(unsigned short *)(p + 20) = 13;     /* callBackCmd */
    *(short *)(p + 22) = 0;               /* param1 */
    *(long *)(p + 24) = 0;                /* param2 */

    /* SoundHeader at offset 28 (24 bytes) */
    *(Ptr *)(p + 28) = samplePtr;          /* samplePtr (NULL = inline) */
    *(long *)(p + 32) = length;            /* sample count */
    *(long *)(p + 36) = sampleRate;        /* Fixed 16.16 */
    *(long *)(p + 40) = 0;                 /* loopStart */
    *(long *)(p + 44) = 0;                 /* loopEnd */
    p[48] = 0;                             /* encode = stdSH */
    p[49] = 60;                            /* baseFrequency = middle C */

    HUnlock(h);
    return h;
}

/* ---- generate test tone ---- */

static unsigned char g_test_tone[1024]; /* static buffer for external ptr tests */
static int g_test_tone_ready = 0;

static void I_EnsureTestTone(void)
{
    int j;
    if (g_test_tone_ready) return;
    /* 1024-sample square wave: ~344 Hz at 11025 Hz, ~93ms duration */
    for (j = 0; j < 1024; j++)
        g_test_tone[j] = (j & 16) ? 200 : 56;
    g_test_tone_ready = 1;
}

/* ---- sound test ladder (9 steps, F5 advances) ---- */

static int snd_test_step = 0;

void I_SoundTest(void)
{
    OSErr err;
    Handle h;
    int step = snd_test_step++;

    doom_log("SNDTEST %d: begin\r", step);

    switch (step) {

    /* ---- Step 0: SysBeep (known working baseline) ---- */
    case 0:
        SysBeep(30);
        doom_log("SNDTEST 0: SysBeep done\r");
        break;

    /* ---- Step 1: SndPlay(NULL) sync, inline data ---- */
    case 1: {
        I_EnsureTestTone();
        h = I_BuildSndHandle(NULL, 1024, 11025L << 16, 1024);
        if (h) {
            /* Copy tone into sampleArea at offset 42 */
            HLock(h);
            memcpy(((unsigned char *)*h) + 50, g_test_tone, 1024);
            HUnlock(h);
            err = SndPlay(NULL, h, false); /* sync, SM-owned channel */
            doom_log("SNDTEST 1: SndPlay(NULL,inline,sync) err=%d\r", (int)err);
            DisposeHandle(h);
        } else {
            doom_log("SNDTEST 1: NewHandle failed\r");
        }
        break;
    }

    /* ---- Step 2: SndPlay(NULL) sync, external samplePtr ---- */
    case 2: {
        I_EnsureTestTone();
        h = I_BuildSndHandle((Ptr)g_test_tone, 1024, 11025L << 16, 0);
        if (h) {
            err = SndPlay(NULL, h, false); /* sync, SM-owned channel */
            doom_log("SNDTEST 2: SndPlay(NULL,extptr,sync) err=%d\r", (int)err);
            DisposeHandle(h);
        } else {
            doom_log("SNDTEST 2: NewHandle failed\r");
        }
        break;
    }

    /* ---- Step 3: SndPlay(freshChan) async, inline data ---- */
    case 3: {
        SndChannelPtr tc = NULL;
        err = SndNewChannel(&tc, sampledSynth, 0x80, NULL);
        doom_log("SNDTEST 3: SndNewChannel err=%d\r", (int)err);
        if (err == noErr && tc) {
            I_EnsureTestTone();
            h = I_BuildSndHandle(NULL, 1024, 11025L << 16, 1024);
            if (h) {
                HLock(h);
                memcpy(((unsigned char *)*h) + 50, g_test_tone, 1024);
                HUnlock(h);
                err = SndPlay(tc, h, true); /* async */
                doom_log("SNDTEST 3: SndPlay(fresh,inline,async) err=%d\r", (int)err);
                /* Wait for playback (~93ms at 11025Hz) then clean up */
                SndDisposeChannel(tc, false); /* false = wait for completion */
                DisposeHandle(h);
                doom_log("SNDTEST 3: disposed\r");
            }
        }
        break;
    }

    /* ---- Step 4: SndPlay(freshChan) async, external samplePtr ---- */
    case 4: {
        SndChannelPtr tc = NULL;
        err = SndNewChannel(&tc, sampledSynth, 0x80, NULL);
        doom_log("SNDTEST 4: SndNewChannel err=%d\r", (int)err);
        if (err == noErr && tc) {
            I_EnsureTestTone();
            h = I_BuildSndHandle((Ptr)g_test_tone, 1024, 11025L << 16, 0);
            if (h) {
                err = SndPlay(tc, h, true); /* async */
                doom_log("SNDTEST 4: SndPlay(fresh,extptr,async) err=%d\r", (int)err);
                SndDisposeChannel(tc, false);
                DisposeHandle(h);
                doom_log("SNDTEST 4: disposed\r");
            }
        }
        break;
    }

    /* ---- Step 5: SndPlay(gameChan[0]) async, inline data ---- */
    case 5: {
        if (mac_num_channels < 1) {
            doom_log("SNDTEST 5: no game channels\r");
            break;
        }
        I_EnsureTestTone();
        h = I_BuildSndHandle(NULL, 1024, 11025L << 16, 1024);
        if (h) {
            HLock(h);
            memcpy(((unsigned char *)*h) + 50, g_test_tone, 1024);
            HUnlock(h);
            err = SndPlay(mac_channels[0].chan, h, true); /* async on game ch */
            doom_log("SNDTEST 5: SndPlay(gameCh0,inline,async) err=%d\r", (int)err);
            /* Don't dispose channel! Just dispose handle after a delay */
            {
                long k;
                for (k = 0; k < 500000L; k++) ; /* spin ~100ms */
            }
            DisposeHandle(h);
            doom_log("SNDTEST 5: handle disposed\r");
        }
        break;
    }

    /* ---- Step 6: SndPlay(gameChan[0]) async, external samplePtr ---- */
    case 6: {
        if (mac_num_channels < 1) {
            doom_log("SNDTEST 6: no game channels\r");
            break;
        }
        I_EnsureTestTone();
        h = I_BuildSndHandle((Ptr)g_test_tone, 1024, 11025L << 16, 0);
        if (h) {
            err = SndPlay(mac_channels[0].chan, h, true); /* async */
            doom_log("SNDTEST 6: SndPlay(gameCh0,extptr,async) err=%d\r", (int)err);
            {
                long k;
                for (k = 0; k < 500000L; k++) ;
            }
            DisposeHandle(h);
            doom_log("SNDTEST 6: handle disposed\r");
        }
        break;
    }

    /* ---- Step 7: SndPlay(gameChan[0]) async, WAD PCM (sfx_pistol) ---- */
    case 7: {
        byte *raw;
        unsigned short srate;
        unsigned long scount;
        sfxinfo_t *sfx;

        if (mac_num_channels < 1) {
            doom_log("SNDTEST 7: no game channels\r");
            break;
        }
        /* Cache sfx_pistol (id=1) */
        sfx = &S_sfx[sfx_pistol];
        if (!sfx->data) {
            if (!I_CacheSfx(sfx_pistol)) {
                doom_log("SNDTEST 7: cache fail\r");
                break;
            }
        }
        raw = (byte *)sfx->data;
        srate  = (unsigned short)(raw[2] | (raw[3] << 8));
        scount = (unsigned long)(raw[4] | (raw[5] << 8) |
                                 (raw[6] << 16) | (raw[7] << 24));

        Z_ChangeTag(raw, PU_SOUND); /* pin during playback */

        h = I_BuildSndHandle((Ptr)(raw + 8), scount,
                              ((unsigned long)srate) << 16, 0);
        if (h) {
            err = SndPlay(mac_channels[0].chan, h, true);
            doom_log("SNDTEST 7: SndPlay(gameCh0,WAD,async) err=%d rate=%u len=%lu\r",
                     (int)err, (unsigned)srate, scount);
            {
                long k;
                for (k = 0; k < 2000000L; k++) ; /* spin ~500ms for full sound */
            }
            DisposeHandle(h);
            Z_ChangeTag(raw, PU_CACHE);
            doom_log("SNDTEST 7: done\r");
        }
        break;
    }

    /* ---- Step 8: Full DOOM engine path: S_StartSound(sfx_pistol) ---- */
    case 8: {
        extern void S_StartSound(void *origin, int sfx_id);
        doom_log("SNDTEST 8: calling S_StartSound(NULL, sfx_pistol)\r");
        S_StartSound(NULL, sfx_pistol);
        doom_log("SNDTEST 8: S_StartSound returned\r");
        break;
    }

    default:
        doom_log("SNDTEST: all %d tests done, resetting to 0\r", step);
        snd_test_step = 0;
        break;
    }
}

/* ---- playback ---- */

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    int i, oldest;
    mac_channel_t *mc;
    sfxinfo_t *sfx;
    byte *raw;
    unsigned short srate;
    unsigned long scount;
    OSErr err;

    (void)sep; (void)pitch; (void)priority;

    if (!opt_sound || mac_num_channels == 0) {
        return -1;
    }

    /* Throttle: limit new starts per frame */
    if (snd_starts_this_frame >= SND_MAX_STARTS_PER_FRAME)
        return -1;

    sfx = &S_sfx[id];

    /* Ensure sound data is cached */
    if (!sfx->data) {
        if (!I_CacheSfx(id)) {
            return -1;
        }
    }

    raw = (byte *)sfx->data;

    /* Find a free channel — drop if all busy (no stealing/queuing) */
    oldest = -1;
    for (i = 0; i < mac_num_channels; i++) {
        if (!mac_channels[i].playing) {
            if (mac_channels[i].cache_ptr) {
                Z_ChangeTag(mac_channels[i].cache_ptr, PU_CACHE);
                mac_channels[i].cache_ptr = NULL;
            }
            oldest = i;
            break;
        }
    }

    if (oldest < 0) return -1;  /* all busy — drop sound */

    mc = &mac_channels[oldest];

    /* Unpin previous cache block */
    if (mc->cache_ptr) {
        Z_ChangeTag(mc->cache_ptr, PU_CACHE);
        mc->cache_ptr = NULL;
    }
    mc->playing = false;

    /* Dispose previous Handle (from last play on this channel) */
    if (mc->snd_handle) {
        DisposeHandle(mc->snd_handle);
        mc->snd_handle = NULL;
    }

    /* If HW is still running (ISStop cleared playing but SndPlay not done),
       dispose+recreate the channel to flush the queue before new SndPlay. */
    if (mc->hw_busy) {
        doom_log("ISS: flush ch%d sfx=%d (hw_busy)\r", oldest, mc->sfx_id);
        if (mc->chan) {
            SndDisposeChannel(mc->chan, true);
            mc->chan = NULL;
        }
        {
            OSErr ch_err = SndNewChannel(&mc->chan, sampledSynth, 0x80,
                                          snd_callback_upp);
            if (ch_err != noErr) {
                doom_log("ISS: SndNewChannel err=%d after flush\r", (int)ch_err);
                mc->hw_busy = false;
                return -1;
            }
        }
        mc->hw_busy = false;
    }

    /* Parse DMX header from raw data */
    srate  = (unsigned short)(raw[2] | (raw[3] << 8));
    scount = (unsigned long)(raw[4] | (raw[5] << 8) |
                             (raw[6] << 16) | (raw[7] << 24));

    /* Pin the cache block so zone allocator won't purge during DMA */
    Z_ChangeTag(raw, PU_SOUND);
    mc->cache_ptr = raw;

    /* Build fresh 44-byte format 1 'snd ' resource with external samplePtr */
    mc->snd_handle = I_BuildSndHandle((Ptr)(raw + 8), scount,
                                       ((unsigned long)srate) << 16, 0);
    if (!mc->snd_handle) {
        Z_ChangeTag(raw, PU_CACHE);
        mc->cache_ptr = NULL;
        return -1;
    }

    /* Play via SndPlay — NO SndDoImmediate calls (they corrupt channels) */
    err = SndPlay(mc->chan, mc->snd_handle, true); /* async */
    if (err != noErr) {
        doom_log("I_StartSound: SndPlay err=%d sfx=%d\r", (int)err, id);
        DisposeHandle(mc->snd_handle);
        mc->snd_handle = NULL;
        Z_ChangeTag(raw, PU_CACHE);
        mc->cache_ptr = NULL;
        return -1;
    }

    mc->sfx_id = id;
    mc->playing = true;
    mc->hw_busy = true;

    snd_starts_this_frame++;
    return oldest;
}

void I_StopSound(int handle)
{
    mac_channel_t *mc;

    if (!opt_sound || handle < 0 || handle >= mac_num_channels)
        return;

    mc = &mac_channels[handle];
    if (!mc->playing)
        return;

    mc->playing = false;

    if (mc->snd_handle) {
        DisposeHandle(mc->snd_handle);
        mc->snd_handle = NULL;
    }
    if (mc->cache_ptr) {
        Z_ChangeTag(mc->cache_ptr, PU_CACHE);
        mc->cache_ptr = NULL;
    }
}

int I_SoundIsPlaying(int handle)
{
    mac_channel_t *mc;

    if (!opt_sound || handle < 0 || handle >= mac_num_channels)
        return 0;

    mc = &mac_channels[handle];

    if (!mc->playing) {
        if (mc->cache_ptr) {
            Z_ChangeTag(mc->cache_ptr, PU_CACHE);
            mc->cache_ptr = NULL;
        }
        return 0;
    }

    return 1;
}

void I_UpdateSound(void)
{
    int i;

    /* Reset per-frame throttle */
    snd_starts_this_frame = 0;

    if (!opt_sound)
        return;

    /* Scan for completed sounds and unpin their cache blocks (main thread) */
    for (i = 0; i < mac_num_channels; i++) {
        if (!mac_channels[i].playing && mac_channels[i].cache_ptr) {
            Z_ChangeTag(mac_channels[i].cache_ptr, PU_CACHE);
            mac_channels[i].cache_ptr = NULL;
        }
    }

}

void I_SubmitSound(void) { }

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    /* no-op: SndDoImmediate (ampCmd) corrupts sampledSynth channels */
    (void)handle; (void)vol; (void)sep; (void)pitch;
}

/* ---- Music stubs (no music support) ---- */
void I_InitMusic(void) { }
void I_ShutdownMusic(void) { }
void I_SetMusicVolume(int volume) { (void)volume; }
void I_PauseSong(int handle) { (void)handle; }
void I_ResumeSong(int handle) { (void)handle; }

int I_RegisterSong(void *data)
{
    (void)data;
    return 1;
}

void I_PlaySong(int handle, int looping)
{
    (void)handle; (void)looping;
}

void I_StopSong(int handle) { (void)handle; }
void I_UnRegisterSong(int handle) { (void)handle; }
