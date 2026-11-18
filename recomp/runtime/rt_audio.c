/* rt_audio.c: the software mixer behind DirectSound and BASS.
 *
 * Voices play either PCM that lives in guest memory (DirectSound buffers,
 * which the game fills itself) or a decoder callback (BASS streams). The
 * platform layer calls rt_audio_mix() for 44.1 kHz stereo s16 output. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rt_audio.h"

static RtVoice voices[RT_MAX_VOICES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static float master = 1.0f;

void rt_audio_lock(void) { pthread_mutex_lock(&lock); }
void rt_audio_unlock(void) { pthread_mutex_unlock(&lock); }

RtVoice *rt_voice_new(void)
{
    rt_audio_lock();
    for (int i = 0; i < RT_MAX_VOICES; i++) {
        if (!voices[i].used) {
            memset(&voices[i], 0, sizeof voices[i]);
            voices[i].used = 1;
            voices[i].vol = 1.0f;
            voices[i].pan_l = voices[i].pan_r = 1.0f;
            rt_audio_unlock();
            return &voices[i];
        }
    }
    rt_audio_unlock();
    static int warned;
    if (!warned++) rt_log("[audio] out of mixer voices (%d)\n", RT_MAX_VOICES);
    return NULL;
}

void rt_voice_free(RtVoice *v)
{
    rt_audio_lock();
    if (v->free_ud) v->free_ud(v->ud);
    memset(v, 0, sizeof *v);
    rt_audio_unlock();
}

void rt_audio_set_master(float v) { master = v; }

/* DirectSound units: hundredths of a dB */
float rt_db_to_gain(int32_t hundredths_db)
{
    if (hundredths_db <= -10000) return 0.0f;
    if (hundredths_db >= 0) return 1.0f;
    return powf(10.0f, (float)hundredths_db / 2000.0f);
}

static inline float pcm_sample(const RtVoice *v, uint32_t frame, int ch)
{
    const uint8_t *p = v->pcm + (size_t)frame * (size_t)v->block;
    if (v->channels == 1) ch = 0;
    if (v->bits == 16) {
        int16_t s;
        memcpy(&s, p + 2 * ch, 2);
        return (float)s;
    }
    return ((float)p[ch] - 128.0f) * 256.0f;
}

static void mix_voice(RtVoice *v, float *acc, int frames)
{
    if (v->slide_left > 0) {
        int n = frames < v->slide_left ? frames : v->slide_left;
        v->vol += (v->slide_to - v->vol) * (float)n / (float)v->slide_left;
        v->slide_left -= n;
        if (!v->slide_left && v->slide_stop) {
            v->playing = 0;
            v->finished = 1;
            return;
        }
    }
    float gl = v->vol * v->pan_l * master, gr = v->vol * v->pan_r * master;
    if (v->decode) {
        /* streaming source: decoder delivers 44.1 kHz stereo s16 */
        int16_t tmp[2048 * 2];
        int done = 0;
        while (done < frames) {
            int want = frames - done > 2048 ? 2048 : frames - done;
            int got = v->decode(v->ud, tmp, want);
            if (got <= 0) {
                if (v->loop && v->rewind && v->rewind(v->ud)) continue;
                v->playing = 0;
                v->finished = 1;
                return;
            }
            for (int i = 0; i < got; i++) {
                acc[2 * (done + i)] += (float)tmp[2 * i] * gl;
                acc[2 * (done + i) + 1] += (float)tmp[2 * i + 1] * gr;
            }
            done += got;
        }
        return;
    }
    if (!v->pcm || !v->bytes || !v->block) {
        v->playing = 0;
        return;
    }
    uint32_t nframes = v->bytes / v->block;
    double step = (double)(v->freq ? v->freq : v->rate) / RT_AUDIO_RATE;
    for (int i = 0; i < frames; i++) {
        if (v->pos >= nframes) {
            if (!v->loop) {
                v->playing = 0;
                v->finished = 1;
                v->pos = 0;
                return;
            }
            v->pos = fmod(v->pos, (double)nframes);
        }
        uint32_t f = (uint32_t)v->pos;
        acc[2 * i] += pcm_sample(v, f, 0) * gl;
        acc[2 * i + 1] += pcm_sample(v, f, 1) * gr;
        v->pos += step;
    }
}

void rt_audio_mix(int16_t *out, int frames)
{
    float acc[4096 * 2];
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        memset(acc, 0, sizeof(float) * 2 * (size_t)n);
        rt_audio_lock();
        for (int k = 0; k < RT_MAX_VOICES; k++)
            if (voices[k].used && voices[k].playing && !voices[k].paused) mix_voice(&voices[k], acc, n);
        rt_audio_unlock();
        for (int i = 0; i < 2 * n; i++) {
            float s = acc[i];
            out[i] = (int16_t)(s > 32767.f ? 32767 : s < -32768.f ? -32768 : s);
        }
        out += 2 * n;
        frames -= n;
    }
}
