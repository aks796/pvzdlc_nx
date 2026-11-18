/* rt_audio.h: software mixer shared by DirectSound and BASS (rt_audio.c). */
#ifndef RT_AUDIO_H
#define RT_AUDIO_H

#include "rt.h"

#ifdef __SWITCH__
#define RT_AUDIO_RATE 48000 /* audout's only rate */
#else
#define RT_AUDIO_RATE 44100
#endif
#define RT_MAX_VOICES 1024

typedef struct RtVoice {
    int used, playing, paused, loop, finished;
    /* PCM source (DirectSound): host pointer into guest memory */
    const uint8_t *pcm;
    uint32_t bytes, block, rate, freq;
    int bits, channels;
    double pos;           /* in source frames */
    /* streaming source (BASS): fills 44.1 kHz stereo s16, returns frames */
    int (*decode)(void *ud, int16_t *out, int frames);
    int (*rewind)(void *ud);
    void (*free_ud)(void *ud);
    void *ud;
    float vol, pan_l, pan_r;
    /* volume slide (BASS_ChannelSlideAttributes): vol moves to slide_to over slide_left output frames */
    float slide_to;
    int slide_left, slide_stop;
} RtVoice;

RtVoice *rt_voice_new(void);
void rt_voice_free(RtVoice *v);
void rt_audio_lock(void);
void rt_audio_unlock(void);
void rt_audio_mix(int16_t *out, int frames);
void rt_audio_set_master(float v);
float rt_db_to_gain(int32_t hundredths_db);

#endif
