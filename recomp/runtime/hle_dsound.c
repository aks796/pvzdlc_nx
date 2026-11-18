/* hle_dsound.c: DirectSound for the framework's sound effects.
 * The game decodes WAV/OGG itself and copies PCM into buffers it Locks; each
 * buffer is guest memory, and gets a mixer voice the first time it plays (the
 * game loads ~200 effects up front and plays them through duplicates, so the
 * loaded originals must not hold voices). Duplicates share the PCM and get
 * their own voice: that is how the framework plays overlapping instances. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_audio.h"
#include "rt_com.h"

#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSBCAPS_PRIMARYBUFFER 0x1u
#define DSBPLAY_LOOPING 0x1u
#define DSBSTATUS_PLAYING 0x1u
#define DSBSTATUS_LOOPING 0x4u

typedef struct DsBuf {
    int refs;
    int primary;
    uint32_t mem, bytes, flags;
    uint8_t fmt[18];      /* WAVEFORMATEX */
    int32_t volume, pan;  /* hundredths of dB / pan units */
    uint32_t freq;        /* SetFrequency (0 = the format's rate) */
    struct DsShared *shared;
    RtVoice *v;
} DsBuf;
typedef struct DsShared { int refs; uint32_t mem; } DsShared;

static uint32_t vt_ds, vt_buf, ds_obj;
static DsBuf *B(uint32_t obj) { return com_impl(obj); }

/* The format into the voice (caller holds the mixer lock) */
static void voice_format(DsBuf *b)
{
    uint16_t ch, bits, align;
    uint32_t rate;
    memcpy(&ch, b->fmt + 2, 2);
    memcpy(&rate, b->fmt + 4, 4);
    memcpy(&align, b->fmt + 12, 2);
    memcpy(&bits, b->fmt + 14, 2);
    b->v->channels = ch ? ch : 1;
    b->v->bits = bits ? bits : 16;
    b->v->rate = rate ? rate : 22050;
    b->v->block = align ? align : (uint32_t)(b->v->channels * b->v->bits / 8);
    b->v->pcm = G2H(b->mem);
    b->v->bytes = b->bytes;
    b->v->freq = b->freq;
}

/* The buffer's voice, made on first use */
static RtVoice *voice(DsBuf *b)
{
    if (b->v || b->primary) return b->v;
    b->v = rt_voice_new();
    if (!b->v) return NULL;
    rt_audio_lock();
    voice_format(b);
    b->v->vol = rt_db_to_gain(b->volume);
    b->v->pan_l = b->pan > 0 ? rt_db_to_gain(-b->pan) : 1.0f;
    b->v->pan_r = b->pan < 0 ? rt_db_to_gain(b->pan) : 1.0f;
    rt_audio_unlock();
    return b->v;
}

static void apply_mix(DsBuf *b)
{
    if (!b->v) return;
    rt_audio_lock();
    b->v->vol = rt_db_to_gain(b->volume);
    b->v->pan_l = b->pan > 0 ? rt_db_to_gain(-b->pan) : 1.0f;
    b->v->pan_r = b->pan < 0 ? rt_db_to_gain(b->pan) : 1.0f;
    rt_audio_unlock();
}

static void set_format(DsBuf *b, uint32_t wfx)
{
    memcpy(b->fmt, G2H(wfx), 16);
    if (!b->v) return;
    rt_audio_lock();
    voice_format(b);
    rt_audio_unlock();
}

static DsBuf *buf_new(uint32_t bytes, uint32_t flags, uint32_t wfx, DsShared *share)
{
    DsBuf *b = calloc(1, sizeof *b);
    b->refs = 1;
    b->flags = flags;
    b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    b->bytes = bytes;
    if (!b->primary) {
        if (share) {
            b->shared = share;
            share->refs++;
        } else {
            b->shared = calloc(1, sizeof *b->shared);
            b->shared->refs = 1;
            b->shared->mem = rt_vm_alloc(0, bytes ? bytes : 1);
        }
        b->mem = b->shared->mem;
    }
    if (wfx) set_format(b, wfx);
    return b;
}

/* ---- IDirectSoundBuffer ---- */
HLE(buf_QueryInterface) { g_wr32(ARG(2), 0); RET(3, E_NOINTERFACE); }
HLE(buf_AddRef) { RET(1, ++B(ARG(0))->refs); }
HLE(buf_Release)
{
    DsBuf *b = B(ARG(0));
    if (!b) RET(1, 0);
    if (--b->refs > 0) RET(1, (uint32_t)b->refs);
    if (b->v) rt_voice_free(b->v);
    if (b->shared && --b->shared->refs == 0) {
        rt_vm_free(b->shared->mem);
        free(b->shared);
    }
    com_unwrap(ARG(0));
    free(b);
    RET(1, 0);
}
HLE(buf_GetCaps)
{
    DsBuf *b = B(ARG(0));
    g_wr32(ARG(1) + 4, b->flags);
    g_wr32(ARG(1) + 8, b->bytes);
    RET(2, DS_OK);
}
HLE(buf_GetCurrentPosition)
{
    DsBuf *b = B(ARG(0));
    uint32_t pos = b->v ? (uint32_t)b->v->pos * (b->v->block ? b->v->block : 1) : 0;
    if (ARG(1)) g_wr32(ARG(1), pos);
    if (ARG(2)) g_wr32(ARG(2), pos);
    RET(3, DS_OK);
}
HLE(buf_GetFormat)
{
    DsBuf *b = B(ARG(0));
    if (ARG(1)) memcpy(G2H(ARG(1)), b->fmt, ARG(2) < 18 ? ARG(2) : 18);
    if (ARG(3)) g_wr32(ARG(3), 18);
    RET(4, DS_OK);
}
HLE(buf_GetVolume) { g_wr32(ARG(1), (uint32_t)B(ARG(0))->volume); RET(2, DS_OK); }
HLE(buf_GetPan) { g_wr32(ARG(1), (uint32_t)B(ARG(0))->pan); RET(2, DS_OK); }
HLE(buf_GetFrequency)
{
    DsBuf *b = B(ARG(0));
    uint32_t rate;
    memcpy(&rate, b->fmt + 4, 4);
    g_wr32(ARG(1), b->freq ? b->freq : rate ? rate : 22050);
    RET(2, DS_OK);
}
HLE(buf_GetStatus)
{
    DsBuf *b = B(ARG(0));
    uint32_t s = 0;
    if (b->v && b->v->playing) s = DSBSTATUS_PLAYING | (b->v->loop ? DSBSTATUS_LOOPING : 0);
    g_wr32(ARG(1), s);
    RET(2, DS_OK);
}
HLE(buf_Lock)
{
    DsBuf *b = B(ARG(0));
    uint32_t off = ARG(1), n = ARG(2), flags = ARG(7);
    if (flags & 2) { off = 0; n = b->bytes; }   /* DSBLOCK_ENTIREBUFFER */
    if (!b->mem || off >= b->bytes) RET(8, DSERR_INVALIDPARAM);
    if (off + n > b->bytes) n = b->bytes - off;
    g_wr32(ARG(3), b->mem + off);
    g_wr32(ARG(4), n);
    if (ARG(5)) g_wr32(ARG(5), 0);
    if (ARG(6)) g_wr32(ARG(6), 0);
    RET(8, DS_OK);
}
HLE(buf_Unlock) { RET(5, DS_OK); }
HLE(buf_Play)
{
    DsBuf *b = B(ARG(0));
    if (voice(b)) {
        rt_audio_lock();
        b->v->loop = (ARG(3) & DSBPLAY_LOOPING) != 0;
        b->v->finished = 0;
        b->v->playing = 1;
        rt_audio_unlock();
    }
    RET(4, DS_OK);
}
HLE(buf_Stop)
{
    DsBuf *b = B(ARG(0));
    if (b->v) b->v->playing = 0;
    RET(1, DS_OK);
}
HLE(buf_SetCurrentPosition)
{
    DsBuf *b = B(ARG(0));
    if (ARG(1) && voice(b) && b->v->block) b->v->pos = (double)(ARG(1) / b->v->block);
    else if (b->v) b->v->pos = 0;
    RET(2, DS_OK);
}
HLE(buf_SetFormat)
{
    set_format(B(ARG(0)), ARG(1));
    RET(2, DS_OK);
}
HLE(buf_SetVolume)
{
    DsBuf *b = B(ARG(0));
    b->volume = (int32_t)ARG(1);
    apply_mix(b);
    RET(2, DS_OK);
}
HLE(buf_SetPan)
{
    DsBuf *b = B(ARG(0));
    b->pan = (int32_t)ARG(1);
    apply_mix(b);
    RET(2, DS_OK);
}
HLE(buf_SetFrequency)
{
    DsBuf *b = B(ARG(0));
    b->freq = ARG(1);
    if (b->v) b->v->freq = ARG(1);
    RET(2, DS_OK);
}
HLE(buf_Restore) { RET(1, DS_OK); }

/* ---- IDirectSound ---- */
HLE(ds_QueryInterface) { g_wr32(ARG(2), 0); RET(3, E_NOINTERFACE); }
HLE(ds_AddRef) { RET(1, 2); }
HLE(ds_Release) { RET(1, 1); }
HLE(ds_CreateSoundBuffer)
{
    uint32_t d = ARG(1), out = ARG(2);
    uint32_t flags = g_rd32(d + 4), bytes = g_rd32(d + 8), wfx = g_rd32(d + 16);
    DsBuf *b = buf_new(bytes, flags, wfx, NULL);
    g_wr32(out, com_wrap(vt_buf, b));
    RET(4, DS_OK);
}
HLE(ds_DuplicateSoundBuffer)
{
    DsBuf *src = B(ARG(1));
    if (!src || src->primary) RET(3, DSERR_INVALIDPARAM);
    DsBuf *b = buf_new(src->bytes, src->flags, 0, src->shared);
    memcpy(b->fmt, src->fmt, sizeof b->fmt);
    uint32_t tmp = rt_heap_alloc(18, 0);
    memcpy(G2H(tmp), src->fmt, 18);
    set_format(b, tmp);
    rt_heap_free(tmp);
    b->volume = src->volume;
    b->pan = src->pan;
    b->freq = src->freq;
    apply_mix(b);
    g_wr32(ARG(2), com_wrap(vt_buf, b));
    RET(3, DS_OK);
}
HLE(ds_GetCaps) { RET(2, DS_OK); }
HLE(ds_SetCooperativeLevel) { RET(3, DS_OK); }
HLE(ds_Compact) { RET(1, DS_OK); }
HLE(ds_GetSpeakerConfig) { g_wr32(ARG(1), 4); RET(2, DS_OK); } /* stereo */
HLE(ds_SetSpeakerConfig) { RET(2, DS_OK); }

HLE(DirectSoundCreate)
{
    if (!vt_ds) {
#define M(n, f) {n, f}
        static const ComMethod ds[11] = {
            M("QueryInterface", ds_QueryInterface), M("AddRef", ds_AddRef), M("Release", ds_Release),
            M("CreateSoundBuffer", ds_CreateSoundBuffer), M("GetCaps", ds_GetCaps),
            M("DuplicateSoundBuffer", ds_DuplicateSoundBuffer), M("SetCooperativeLevel", ds_SetCooperativeLevel),
            M("Compact", ds_Compact), M("GetSpeakerConfig", ds_GetSpeakerConfig),
            M("SetSpeakerConfig", ds_SetSpeakerConfig), M("Initialize", NULL)};
        static const ComMethod buf[21] = {
            M("QueryInterface", buf_QueryInterface), M("AddRef", buf_AddRef), M("Release", buf_Release),
            M("GetCaps", buf_GetCaps), M("GetCurrentPosition", buf_GetCurrentPosition), M("GetFormat", buf_GetFormat),
            M("GetVolume", buf_GetVolume), M("GetPan", buf_GetPan), M("GetFrequency", buf_GetFrequency),
            M("GetStatus", buf_GetStatus), M("Initialize", NULL), M("Lock", buf_Lock), M("Play", buf_Play),
            M("SetCurrentPosition", buf_SetCurrentPosition), M("SetFormat", buf_SetFormat),
            M("SetVolume", buf_SetVolume), M("SetPan", buf_SetPan), M("SetFrequency", buf_SetFrequency),
            M("Stop", buf_Stop), M("Unlock", buf_Unlock), M("Restore", buf_Restore)};
#undef M
        vt_ds = com_vtable("IDirectSound", ds, 11);
        vt_buf = com_vtable("IDirectSoundBuffer", buf, 21);
        static int impl;
        ds_obj = com_wrap(vt_ds, &impl);
    }
    rt_log("[dsound] DirectSoundCreate\n");
    g_wr32(ARG(1), ds_obj);
    RET(3, DS_OK);
}

const HleEntry hle_dsound[] = {
    {"dsound.dll", "DirectSoundCreate", DirectSoundCreate},
    {NULL, NULL, NULL}};
