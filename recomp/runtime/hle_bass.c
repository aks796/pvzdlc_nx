/* hle_bass.c: bass.dll (BASS 2.x API as the framework's BassLoader uses it).
 * OGG streams (all the DLC's music) decode with stb_vorbis; MO3 music (the base
 * game's mainmusic/mainmusic_hihats/dlcmusic) plays through libopenmpt. Lawn's
 * Music.cpp drives the modules hard: order jumps (MAKEMUSICPOS), per-channel
 * volumes to pick a tune's tracks out of mainmusic.mo3, BPM nudges to keep the
 * drums/hihats copies in step with the main copy, and GetOrderPosition polling. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "rt_audio.h"

#include "rt_vorbis.h"
#include "libopenmpt/libopenmpt.h"
#include "libopenmpt/libopenmpt_ext.h"

#define MAXH 4096
static struct {
    int used, playing, kind; /* kind: 1 music, 2 stream, 3 sample, 4 channel */
    int sliding_vol;
    uint32_t vol;
    int32_t pan;
    RtVoice *v;
} h[MAXH];

#define SBUF 1024
typedef struct {
    stb_vorbis *vf;
    uint8_t *owned; /* file data read from disk (NULL for in-memory streams) */
    uint32_t rate;  /* source rate; anything but 44.1 kHz goes through the resampler */
    uint32_t channels;
    int16_t buf[2 * SBUF];
    int have;       /* decoded frames in buf */
    double pos;     /* read position in buf, in source frames */
} Stream;

/* Refill buf, keeping the last frame so interpolation can straddle the refill */
static int refill(Stream *s)
{
    if (s->have > 0) {
        s->buf[0] = s->buf[2 * (s->have - 1)];
        s->buf[1] = s->buf[2 * (s->have - 1) + 1];
        s->pos -= s->have - 1;
        s->have = 1;
    }
    int got = stb_vorbis_get_samples_short_interleaved(s->vf, 2, s->buf + 2 * s->have, (SBUF - s->have) * 2);
    if (got > 0) s->have += got;
    return got;
}
static int stream_decode(void *ud, int16_t *out, int frames)
{
    Stream *s = ud;
    if (s->rate == RT_AUDIO_RATE) return stb_vorbis_get_samples_short_interleaved(s->vf, 2, out, frames * 2);
    double step = (double)s->rate / RT_AUDIO_RATE;
    int n = 0;
    while (n < frames) {
        while ((int)s->pos + 1 >= s->have)
            if (refill(s) <= 0) return n;
        int i = (int)s->pos;
        float t = (float)(s->pos - i);
        for (int ch = 0; ch < 2; ch++) {
            float a = s->buf[2 * i + ch], b = s->buf[2 * i + 2 + ch];
            out[2 * n + ch] = (int16_t)(a + (b - a) * t);
        }
        s->pos += step;
        n++;
    }
    return n;
}
static int stream_rewind(void *ud)
{
    Stream *s = ud;
    s->have = 0;
    s->pos = 0;
    return stb_vorbis_seek_start(s->vf);
}
static void stream_free(void *ud)
{
    Stream *s = ud;
    stb_vorbis_close(s->vf);
    free(s->owned);
    free(s);
}
/* Restart a stream from the top; the mixer thread may be decoding it */
static void rewind_voice(RtVoice *v)
{
    rt_audio_lock();
    stream_rewind(v->ud);
    rt_audio_unlock();
}
/* An MO3/MOD module. The mixer thread decodes it under the mixer lock, so every
 * HLE call that touches the module takes that lock too. */
typedef struct {
    openmpt_module_ext *ext;
    openmpt_module *mod;
    openmpt_module_ext_interface_interactive ia;
    openmpt_module_ext_interface_interactive3 ia3;
    int has_ia, has_ia3;
    uint32_t pscaler, amplify, pansep;
    /* the channel volumes the game set (BASS_MUSIC_ATTRIB_VOL_CHAN; -1 not set): Lawn's Music picks a tune's
     * tracks out of mainmusic.mo3 by them (the main copy its melody, the drums / hi-hats copies theirs), then
     * jumps to the tune's order. libopenmpt resets every channel's volume on a position change (a jump, a loop
     * back, a rewind), which put the drums and hi-hats in the main copy for good: every level played at its
     * horde intensity. They are put back before each block is decoded (BASS keeps them over a jump). */
    float chan_vol[64];
} Music;

static void music_keep_channel_volumes(Music *m)
{
    if (!m->has_ia) return;
    for (int ch = 0; ch < 64; ch++)
        if (m->chan_vol[ch] >= 0.f && fabs(m->ia.get_channel_volume(m->ext, ch) - (double)m->chan_vol[ch]) > 1e-4)
            m->ia.set_channel_volume(m->ext, ch, (double)m->chan_vol[ch]);
}
static int music_decode(void *ud, int16_t *out, int frames)
{
    Music *m = ud;
    music_keep_channel_volumes(m);
    return (int)openmpt_module_read_interleaved_stereo(m->mod, RT_AUDIO_RATE, (size_t)frames, out);
}
static int music_rewind(void *ud)
{
    openmpt_module_set_position_order_row(((Music *)ud)->mod, 0, 0);
    return 1;
}
static void music_free(void *ud)
{
    Music *m = ud;
    openmpt_module_ext_destroy(m->ext);
    free(m);
}
static void music_log(const char *message, void *user)
{
    (void)user;
    if (rt_verbose) rt_log("[openmpt] %s\n", message);
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t new_handle(int kind)
{
    pthread_mutex_lock(&lock);
    for (uint32_t i = 1; i < MAXH; i++) {
        if (!h[i].used) {
            h[i].used = 1;
            h[i].playing = 0;
            h[i].kind = kind;
            h[i].vol = 100;
            h[i].pan = 0;
            h[i].v = NULL;
            h[i].sliding_vol = 0;
            pthread_mutex_unlock(&lock);
            return 0x10000u + i;
        }
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
static int idx(uint32_t handle) { return handle > 0x10000u && handle < 0x10000u + MAXH ? (int)(handle - 0x10000u) : 0; }
static void free_handle(uint32_t handle) { h[idx(handle)].used = 0; }

HLE(BASS_GetVersion) { RET(0, 0x02030000u); }
HLE(BASS_Init)
{
    rt_log("[bass] Init(device %d, %u Hz) (OGG via stb_vorbis, MO3 via libopenmpt)\n", (int)ARG(0), (unsigned)ARG(1));
    RET(5, 1);
}
HLE(BASS_Free) { RET(0, 0); }
HLE(BASS_Stop) { RET(0, 1); }
HLE(BASS_Start) { RET(0, 1); }
HLE(BASS_SetGlobalVolumes) { RET(3, 0); }
HLE(BASS_SetVolume) { RET(1, 1); }
HLE(BASS_GetVolume) { RET(0, 100); }
HLE(BASS_GetInfo)
{
    uint32_t p = ARG(0), size = g_rd32(p);
    if (size > 4 && size < 256) memset(G2H(p + 4), 0, size - 4);
    RET(1, 1);
}
HLE(BASS_SetConfig) { RET(2, ARG(1)); }
HLE(BASS_GetConfig) { RET(1, 0); }
HLE(BASS_ErrorGetCode) { RET(0, 0); }

static void apply(int i)
{
    RtVoice *v = h[i].v;
    if (!v) return;
    rt_audio_lock();
    if (!v->slide_left) v->vol = (float)h[i].vol / 100.0f;
    v->pan_l = h[i].pan > 0 ? (100.0f - (float)h[i].pan) / 100.0f : 1.0f;
    v->pan_r = h[i].pan < 0 ? (100.0f + (float)h[i].pan) / 100.0f : 1.0f;
    v->playing = h[i].playing;
    v->paused = 0;
    rt_audio_unlock();
}
static Music *music_of(int i) { return h[i].kind == 1 && h[i].v ? h[i].v->ud : NULL; }

HLE(BASS_ChannelStop)
{
    int i = idx(ARG(0));
    h[i].playing = 0;
    if (h[i].v && h[i].kind == 2) rewind_voice(h[i].v); /* music keeps its position; the game sets one before playing */
    apply(i);
    RET(1, 1);
}
HLE(BASS_ChannelPlay)
{
    int i = idx(ARG(0));
    if (getenv("RT_BASS_VOL_LOG") && h[i].kind != 3 && h[i].kind != 4) rt_log("[bassvol] play %05x kind %d restart %u vol %u\n", (unsigned)ARG(0), h[i].kind, (unsigned)ARG(1), (unsigned)h[i].vol);
    if (ARG(1) && h[i].v) {
        if (h[i].kind == 1) {
            rt_audio_lock();
            music_rewind(h[i].v->ud);
            rt_audio_unlock();
        } else {
            rewind_voice(h[i].v);
        }
    }
    if (h[i].v) h[i].v->finished = 0;
    h[i].playing = 1;
    apply(i);
    RET(2, 1);
}
HLE(BASS_ChannelPause)
{
    int i = idx(ARG(0));
    h[i].playing = 0;
    apply(i);
    RET(1, 1);
}
HLE(BASS_ChannelSetAttributes)
{
    int i = idx(ARG(0));
    static int log_on = -1;
    if (log_on < 0) log_on = getenv("RT_BASS_VOL_LOG") != NULL;
    if (log_on && (int32_t)ARG(2) >= 0 && h[i].vol != ARG(2) && h[i].kind != 3 && h[i].kind != 4)
        rt_log("[bassvol] %05x kind %d vol %u -> %u\n", (unsigned)ARG(0), h[i].kind, (unsigned)h[i].vol, (unsigned)ARG(2));
    if ((int32_t)ARG(2) >= 0) h[i].vol = ARG(2);
    if ((int32_t)ARG(3) >= -100 && (int32_t)ARG(3) <= 100) h[i].pan = (int32_t)ARG(3);
    apply(i);
    RET(4, 1);
}
HLE(BASS_ChannelGetAttributes)
{
    if (ARG(1)) g_wr32(ARG(1), 44100);
    if (ARG(2)) g_wr32(ARG(2), h[idx(ARG(0))].vol);
    if (ARG(3)) g_wr32(ARG(3), 0);
    RET(4, 1);
}
/* QWORD pos: music takes MAKEMUSICPOS(order,row) = 0x80000000|MAKELONG(order,row), or bytes of
 * 44.1 kHz s16 stereo output; streams take bytes of their own decoded format */
HLE(BASS_ChannelSetPosition)
{
    int i = idx(ARG(0));
    uint32_t pos = ARG(1);
    Music *m = music_of(i);
    if (m) {
        if (rt_verbose) rt_log("[bass] music %05x -> order %u row %u\n", (unsigned)ARG(0), (unsigned)(pos & 0xffff),
                               (unsigned)((pos >> 16) & 0x7fff));
        rt_audio_lock();
        if (pos & 0x80000000u)
            openmpt_module_set_position_order_row(m->mod, (int32_t)(pos & 0xffff), (int32_t)((pos >> 16) & 0x7fff));
        else
            openmpt_module_set_position_seconds(m->mod, (double)pos / (RT_AUDIO_RATE * 4.0));
        music_keep_channel_volumes(m);
        if (getenv("RT_BASS_VOL_LOG") && m->has_ia) rt_log("[bassvol] %05x after the jump: chan 29 volume %.2f\n", (unsigned)ARG(0), m->ia.get_channel_volume(m->ext, 29));
        h[i].v->finished = 0;
        rt_audio_unlock();
    } else if (h[i].kind == 2 && h[i].v) {
        Stream *st = h[i].v->ud;
        rt_audio_lock();
        stb_vorbis_seek(st->vf, pos / (2u * st->channels));
        st->have = 0;
        st->pos = 0;
        h[i].v->finished = 0;
        rt_audio_unlock();
    }
    RET(3, 1);
}
HLE(BASS_ChannelGetPosition)
{
    int i = idx(ARG(0));
    uint64_t bytes = 0;
    Music *m = music_of(i);
    if (m) {
        rt_audio_lock();
        bytes = (uint64_t)(openmpt_module_get_position_seconds(m->mod) * RT_AUDIO_RATE) * 4;
        rt_audio_unlock();
    } else if (h[i].kind == 2 && h[i].v) {
        Stream *st = h[i].v->ud;
        rt_audio_lock();
        int64_t f = stb_vorbis_get_playback_sample_offset(st->vf);
        rt_audio_unlock();
        if (f > 0) bytes = (uint64_t)f * 2u * st->channels;
    }
    c->edx = (uint32_t)(bytes >> 32);
    RET(1, (uint32_t)bytes);
}
HLE(BASS_ChannelGetLength)
{
    c->edx = 0;
    RET(1, 44100 * 4 * 60);
}
HLE(BASS_ChannelSetFlags)
{
    int i = idx(ARG(0));
    Music *m = music_of(i);
    if (m) {
        rt_audio_lock();
        openmpt_module_set_repeat_count(m->mod, (ARG(1) & 4) ? -1 : 0); /* BASS_MUSIC_LOOP */
        rt_audio_unlock();
    } else if (h[i].v) {
        h[i].v->loop = (ARG(1) & 4) != 0; /* BASS_SAMPLE_LOOP */
    }
    RET(2, 1);
}
HLE(BASS_ChannelIsActive)
{
    int i = idx(ARG(0));
    if (h[i].v && h[i].v->finished) h[i].playing = 0;
    if (h[i].v && h[i].kind == 1 && !h[i].v->playing && h[i].playing) h[i].playing = 0;
    RET(1, h[i].playing ? 1 : 0);
}
/* (handle, freq, volume, pan, time ms): volume -2 fades out and stops; -1 leaves it */
HLE(BASS_ChannelSlideAttributes)
{
    int i = idx(ARG(0));
    if (getenv("RT_BASS_VOL_LOG")) rt_log("[bassvol] slide %05x kind %d vol %d over %u ms\n", (unsigned)ARG(0), h[i].kind, (int)ARG(2), (unsigned)ARG(4));
    int32_t vol = (int32_t)ARG(2);
    uint32_t ms = ARG(4);
    if (vol < 0 && vol != -2) RET(5, 1);
    uint32_t target = vol == -2 ? 0 : (uint32_t)vol;
    h[i].vol = target;
    RtVoice *v = h[i].v;
    if (!v || !ms) {
        apply(i);
        if (vol == -2) h[i].playing = 0, apply(i);
        RET(5, 1);
    }
    rt_audio_lock();
    v->slide_to = (float)target / 100.0f;
    v->slide_left = (int)((uint64_t)ms * RT_AUDIO_RATE / 1000);
    v->slide_stop = vol == -2;
    rt_audio_unlock();
    RET(5, 1);
}
HLE(BASS_ChannelIsSliding)
{
    int i = idx(ARG(0));
    RET(1, h[i].v && h[i].v->slide_left > 0 ? 2u : 0u); /* BASS_SLIDE_VOL */
}
HLE(BASS_ChannelGetLevel) { RET(1, 0); }
HLE(BASS_ChannelSetFX) { RET(3, new_handle(5)); }
HLE(BASS_ChannelRemoveFX) { RET(2, 1); }
HLE(BASS_ChannelGetData) { RET(3, 0xffffffffu); }
HLE(BASS_ChannelPreBuf) { RET(2, 1); }
HLE(BASS_ChannelSetSync) { RET(6, new_handle(6)); }
HLE(BASS_ChannelRemoveSync) { RET(2, 1); }
HLE(BASS_FXSetParameters) { RET(2, 1); }
HLE(BASS_FXGetParameters) { RET(2, 1); }

/* (mem, file, offset, length, flags, freq). Lawn loads from memory with length 0 (BASS sizes MO3
 * itself), then frees the buffer; the size comes from the heap block, and libopenmpt copies the data. */
HLE(BASS_MusicLoad)
{
    uint32_t mem = ARG(0), file = ARG(1), off = ARG(2), len = ARG(3), flags = ARG(4);
    const uint8_t *data = NULL;
    uint8_t *owned = NULL;
    char name[300] = "(memory)";
    if (mem) {
        if (!len || len == 0xffffffffu) { /* the DLC passes -1 */
            /* the buffer's end: its heap block, else its VM region, else a capped guess (MO3 is read
             * by its own sizes, so slack past the end is never touched) */
            uint32_t base, size, hs = rt_heap_size(file);
            const char *how = "heap block";
            if (hs != 0xffffffffu && hs) len = hs;
            else if (rt_vm_query(file, &base, &size)) len = base + size - file, how = "VM region";
            else len = 64u << 20, how = "guess";
            if (len > RT_HLE_BASE - file) len = RT_HLE_BASE - file;
            len = len > off ? len - off : 0;
            if (rt_verbose) rt_log("[bass] MusicLoad buffer %08x: %u bytes from its %s\n", (unsigned)file, (unsigned)len, how);
        }
        data = (const uint8_t *)G2H(file) + off;
    } else {
        char hp[1024];
        rt_a2u(file, name, sizeof name);
        rt_host_path(name, hp, sizeof hp);
        FILE *f = fopen(hp, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f) - (long)off;
            fseek(f, (long)off, SEEK_SET);
            if (!len || len > (uint32_t)n) len = (uint32_t)n;
            owned = malloc(len ? len : 1);
            if (fread(owned, 1, len, f) != len) len = 0;
            fclose(f);
            data = owned;
        }
    }
    int err = 0;
    const char *msg = NULL;
    openmpt_module_ext *ext = data && len ? openmpt_module_ext_create_from_memory(data, len, music_log, NULL, NULL,
                                                                                 NULL, &err, &msg, NULL)
                                          : NULL;
    free(owned);
    if (!ext) {
        rt_log("[bass] MusicLoad(%s, %u bytes) failed: %s\n", name, (unsigned)len, msg ? msg : "no data");
        if (msg) openmpt_free_string(msg);
        RET(6, 0);
    }
    Music *m = calloc(1, sizeof *m);
    for (int ch = 0; ch < 64; ch++) m->chan_vol[ch] = -1.f; /* (not set by the game: the module's own) */
    m->ext = ext;
    m->mod = openmpt_module_ext_get_module(ext);
    m->has_ia = openmpt_module_ext_get_interface(ext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE, &m->ia, sizeof m->ia);
    m->has_ia3 = openmpt_module_ext_get_interface(ext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE3, &m->ia3, sizeof m->ia3);
    m->pscaler = 1;
    m->amplify = 50;
    m->pansep = 50;
    openmpt_module_set_repeat_count(m->mod, (flags & 4) ? -1 : 0);
    if (flags & 0x10000) openmpt_module_set_render_param(m->mod, OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH, 1);
    else openmpt_module_set_render_param(m->mod, OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH, 2); /* BASS: linear */
    uint32_t hd = new_handle(1);
    int i = idx(hd);
    h[i].v = rt_voice_new();
    if (!h[i].v) {
        music_free(m);
        free_handle(hd);
        RET(6, 0);
    }
    rt_audio_lock();
    h[i].v->decode = music_decode;
    h[i].v->rewind = music_rewind;
    h[i].v->free_ud = music_free;
    h[i].v->ud = m;
    rt_audio_unlock();
    rt_log("[bass] MusicLoad(%s, %u bytes): %d channels, %d orders, %.0f BPM\n", name, (unsigned)len,
           (int)openmpt_module_get_num_channels(m->mod), (int)openmpt_module_get_num_orders(m->mod),
           openmpt_module_get_current_tempo2(m->mod));
    RET(6, hd);
}
HLE(BASS_MusicFree)
{
    int i = idx(ARG(0));
    if (h[i].v) rt_voice_free(h[i].v);
    h[i].v = NULL;
    free_handle(ARG(0));
    RET(1, 0);
}
#define ATTRIB_AMPLIFY 0
#define ATTRIB_PANSEP 1
#define ATTRIB_PSCALER 2
#define ATTRIB_BPM 3
#define ATTRIB_SPEED 4
#define ATTRIB_VOL_GLOBAL 5
#define ATTRIB_VOL_CHAN 0x100
#define ATTRIB_VOL_INST 0x200
HLE(BASS_MusicGetAttribute)
{
    Music *m = music_of(idx(ARG(0)));
    uint32_t a = ARG(1), r = 0xffffffffu;
    if (!m) RET(2, r);
    rt_audio_lock();
    if (a == ATTRIB_AMPLIFY) r = m->amplify;
    else if (a == ATTRIB_PANSEP) r = m->pansep;
    else if (a == ATTRIB_PSCALER) r = m->pscaler;
    else if (a == ATTRIB_BPM) r = (uint32_t)(openmpt_module_get_current_tempo2(m->mod) + 0.5);
    else if (a == ATTRIB_SPEED) r = (uint32_t)openmpt_module_get_current_speed(m->mod);
    else if (a == ATTRIB_VOL_GLOBAL && m->has_ia) r = (uint32_t)(m->ia.get_global_volume(m->ext) * 64.0 + 0.5);
    else if (a >= ATTRIB_VOL_CHAN && a < ATTRIB_VOL_INST && m->has_ia)
        r = (uint32_t)(m->ia.get_channel_volume(m->ext, (int32_t)(a - ATTRIB_VOL_CHAN)) * 100.0 + 0.5);
    rt_audio_unlock();
    RET(2, r);
}
HLE(BASS_MusicSetAttribute)
{
    Music *m = music_of(idx(ARG(0)));
    uint32_t a = ARG(1), v = ARG(2);
    if (!m) RET(3, 0xffffffffu);
    rt_audio_lock();
    if (a == ATTRIB_AMPLIFY) {
        /* BASS amplify is 0-100 with 50 as unity */
        m->amplify = v;
        double g = v ? 2000.0 * log10((double)v / 50.0) : -10000.0;
        openmpt_module_set_render_param(m->mod, OPENMPT_MODULE_RENDER_MASTERGAIN_MILLIBEL, (int32_t)g);
    } else if (a == ATTRIB_PANSEP) {
        m->pansep = v;
        openmpt_module_set_render_param(m->mod, OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT, (int32_t)(v * 2));
    } else if (a == ATTRIB_PSCALER) {
        m->pscaler = v ? v : 1;
    } else if (m->has_ia) {
        if (a == ATTRIB_BPM) {
            if (m->has_ia3) m->ia3.set_current_tempo2(m->ext, (double)v);
        }
        else if (a == ATTRIB_SPEED) m->ia.set_current_speed(m->ext, (int32_t)v);
        else if (a == ATTRIB_VOL_GLOBAL) m->ia.set_global_volume(m->ext, (double)v / 64.0);
        else if (a >= ATTRIB_VOL_CHAN && a < ATTRIB_VOL_INST) {
            int ch = (int)(a - ATTRIB_VOL_CHAN);
            double cv = (double)(v > 100 ? 100 : v) / 100.0;
            m->ia.set_channel_volume(m->ext, ch, cv);
            if (ch < 64) m->chan_vol[ch] = (float)cv;
        }
    }
    rt_audio_unlock();
    RET(3, v);
}
HLE(BASS_MusicGetOrders)
{
    Music *m = music_of(idx(ARG(0)));
    RET(1, m ? (uint32_t)openmpt_module_get_num_orders(m->mod) : 0xffffffffu);
}
/* MAKELONG(order, row * PSCALER) */
HLE(BASS_MusicGetOrderPosition)
{
    Music *m = music_of(idx(ARG(0)));
    if (!m) RET(1, 0xffffffffu);
    rt_audio_lock();
    uint32_t order = (uint32_t)openmpt_module_get_current_order(m->mod);
    uint32_t row = (uint32_t)openmpt_module_get_current_row(m->mod) * m->pscaler;
    rt_audio_unlock();
    RET(1, (order & 0xffff) | (row << 16));
}
HLE(BASS_StreamCreateFile)
{
    uint32_t mem = ARG(0), file = ARG(1), off = ARG(2), len = ARG(3), flags = ARG(4);
    Stream *st = calloc(1, sizeof *st);
    const uint8_t *data = NULL;
    if (mem) {
        data = (const uint8_t *)G2H(file) + off;
    } else {
        char g[300], hp[1024];
        rt_a2u(file, g, sizeof g);
        rt_host_path(g, hp, sizeof hp);
        FILE *f = fopen(hp, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f) - (long)off;
            fseek(f, (long)off, SEEK_SET);
            if (!len || len > (uint32_t)n) len = (uint32_t)n;
            st->owned = malloc(len);
            if (fread(st->owned, 1, len, f) != len) len = 0;
            fclose(f);
            data = st->owned;
        }
        if (rt_verbose) rt_log("[bass] StreamCreateFile(%s)\n", g);
    }
    int err = 0;
    st->vf = data && len ? stb_vorbis_open_memory(data, (int)len, &err, NULL) : NULL;
    if (!st->vf) {
        rt_log("[bass] stream isn't OGG Vorbis we can decode (error %d); silent\n", err);
        free(st->owned);
        free(st);
        RET(5, new_handle(2));
    }
    stb_vorbis_info vi = stb_vorbis_get_info(st->vf);
    st->rate = vi.sample_rate ? vi.sample_rate : RT_AUDIO_RATE;
    st->channels = vi.channels > 0 ? (uint32_t)vi.channels : 2;
    if (rt_verbose) rt_log("[bass] stream %u bytes, %u Hz, %d ch%s\n", (unsigned)len, vi.sample_rate, vi.channels,
                           flags & 4 ? ", looping" : "");
    uint32_t hd = new_handle(2);
    int i = idx(hd);
    h[i].v = rt_voice_new();
    if (h[i].v) {
        rt_audio_lock();
        h[i].v->decode = stream_decode;
        h[i].v->rewind = stream_rewind;
        h[i].v->free_ud = stream_free;
        h[i].v->ud = st;
        h[i].v->loop = (flags & 4) != 0;
        rt_audio_unlock();
    }
    RET(5, hd);
}
HLE(BASS_StreamFree)
{
    int i = idx(ARG(0));
    if (h[i].v) rt_voice_free(h[i].v);
    h[i].v = NULL;
    free_handle(ARG(0));
    RET(1, 0);
}
HLE(BASS_PluginLoad) { RET(2, new_handle(7)); }
HLE(BASS_SampleLoad) { RET(6, new_handle(3)); }
HLE(BASS_SampleFree)
{
    free_handle(ARG(0));
    RET(1, 1);
}
HLE(BASS_SampleSetInfo) { RET(2, 1); }
HLE(BASS_SampleGetInfo)
{
    memset(G2H(ARG(1)), 0, 40);
    g_wr32(ARG(1), 44100);
    g_wr32(ARG(1) + 4, 100);
    RET(2, 1);
}
HLE(BASS_SampleGetChannel) { RET(2, new_handle(4)); }
HLE(BASS_SampleStop) { RET(1, 1); }

const HleEntry hle_bass[] = {
#define B(n) {"bass.dll", #n, n}
    B(BASS_GetVersion), B(BASS_Init), B(BASS_Free), B(BASS_Stop), B(BASS_Start), B(BASS_SetGlobalVolumes),
    B(BASS_SetVolume), B(BASS_GetVolume), B(BASS_GetInfo), B(BASS_SetConfig), B(BASS_GetConfig),
    B(BASS_ErrorGetCode), B(BASS_ChannelStop), B(BASS_ChannelPlay), B(BASS_ChannelPause),
    B(BASS_ChannelSetAttributes), B(BASS_ChannelGetAttributes), B(BASS_ChannelSetPosition),
    B(BASS_ChannelGetPosition), B(BASS_ChannelGetLength), B(BASS_ChannelSetFlags), B(BASS_ChannelIsActive),
    B(BASS_ChannelSlideAttributes), B(BASS_ChannelIsSliding), B(BASS_ChannelGetLevel), B(BASS_ChannelSetFX),
    B(BASS_ChannelRemoveFX), B(BASS_ChannelGetData), B(BASS_ChannelPreBuf), B(BASS_ChannelSetSync),
    B(BASS_ChannelRemoveSync), B(BASS_FXSetParameters), B(BASS_FXGetParameters), B(BASS_MusicLoad),
    B(BASS_MusicFree), B(BASS_MusicGetAttribute), B(BASS_MusicSetAttribute), B(BASS_MusicGetOrders),
    B(BASS_MusicGetOrderPosition), B(BASS_StreamCreateFile), B(BASS_StreamFree), B(BASS_PluginLoad),
    B(BASS_SampleLoad), B(BASS_SampleFree), B(BASS_SampleSetInfo), B(BASS_SampleGetInfo),
    B(BASS_SampleGetChannel), B(BASS_SampleStop),
#undef B
    {NULL, NULL, NULL}};
