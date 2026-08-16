/* audio_synth.c -- the procedural-PCM SoundEngine implementation. Ported
 * verbatim from SHANKPIT's packages/audio/audio.c (the actual synthesis
 * math, mixing, and spatial-gain calculation are unchanged) and wrapped
 * behind the SoundEngine vtable audio.h defines, per NORTHSTAR.md §7's
 * own instruction: wrap first, then port, so a future real-asset backend
 * is a second implementation behind the same interface, not a rewrite.
 *
 * Single global instance, deliberately: there is exactly one audio
 * device on a real machine, so this backend doesn't support (and doesn't
 * pretend to support) multiple concurrent SoundEngine instances -- the
 * `self` parameter on every vtable function is accepted (interface
 * conformance) but unused (single static instance underneath). A future
 * backend that genuinely needs per-instance state can use `self` for
 * real; this one doesn't need to.
 */
#include "audio.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE    22050
#define NUM_CHANNELS   2
#define BUFFER_FRAMES  512

#define MAX_SOUNDS     12       /* 0-5 weapons, 6-10 footstep pentatonic notes */
#define MAX_MIX        16       /* simultaneous voices */

typedef struct {
    int16_t *buf;
    int      len;
} SoundBuf;

typedef struct {
    const SoundBuf *snd;
    int             pos;
    float           lgain;
    float           rgain;
    int             active;
} Voice;

static SoundBuf          g_snd[MAX_SOUNDS];
static Voice              g_voices[MAX_MIX];
static SDL_AudioDeviceID g_dev;
static SDL_mutex         *g_mutex;

static float fclampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static SoundBuf buf_alloc(int frames) {
    SoundBuf b;
    b.len = frames;
    b.buf = (int16_t *)calloc(frames * NUM_CHANNELS, sizeof(int16_t));
    return b;
}

static void buf_write(SoundBuf *b, int i, float l, float r) {
    b->buf[i * 2]     = (int16_t)(fclampf(l, -1.0f, 1.0f) * 32767.0f);
    b->buf[i * 2 + 1] = (int16_t)(fclampf(r, -1.0f, 1.0f) * 32767.0f);
}

/* 808 kick: pitch-sweep sine + click transient */
static SoundBuf synth_kick(float start_hz, float end_hz, float dur, float amp) {
    int n = (int)(SAMPLE_RATE * dur);
    SoundBuf b = buf_alloc(n);
    float phase = 0.0f;
    const float pi2 = 6.2831853f;
    for (int i = 0; i < n; i++) {
        float t   = (float)i / n;
        float hz  = start_hz + (end_hz - start_hz) * t;
        phase    += (hz / SAMPLE_RATE) * pi2;
        if (phase > pi2) phase -= pi2;
        float env = amp * expf(-t * 7.0f);
        if (i < 6) env += 0.45f * (1.0f - (float)i * 0.167f);
        float s = sinf(phase) * env;
        buf_write(&b, i, s, s);
    }
    return b;
}

/* Noise burst: hi-hat / snare style */
static SoundBuf synth_noise(float dur, float amp, float decay) {
    int n = (int)(SAMPLE_RATE * dur);
    SoundBuf b = buf_alloc(n);
    uint32_t rng = 0xABCD1234u;
    for (int i = 0; i < n; i++) {
        rng   = rng * 1664525u + 1013904223u;
        float noise = (float)(int32_t)rng / 2147483648.0f;
        float t   = (float)i / n;
        float env = amp * expf(-t * decay);
        buf_write(&b, i, noise * env, noise * env);
    }
    return b;
}

/* Pure sine tone with decay envelope */
static SoundBuf synth_tone(float hz, float dur, float amp, float decay) {
    int n = (int)(SAMPLE_RATE * dur);
    SoundBuf b = buf_alloc(n);
    float phase = 0.0f;
    float dp = (hz / SAMPLE_RATE) * 6.2831853f;
    for (int i = 0; i < n; i++) {
        float t   = (float)i / n;
        float env = amp * expf(-t * decay);
        if (i < 32) env *= (float)i / 32.0f;
        float s = sinf(phase) * env;
        phase  += dp;
        if (phase > 6.2831853f) phase -= 6.2831853f;
        buf_write(&b, i, s, s);
    }
    return b;
}

/* Layer two sounds into a new buffer (additive mix, clipped). */
static SoundBuf synth_layer(SoundBuf *a, SoundBuf *b) {
    int n = a->len > b->len ? a->len : b->len;
    SoundBuf out = buf_alloc(n);
    for (int i = 0; i < n; i++) {
        float al = (i < a->len) ? a->buf[i*2]   / 32767.0f : 0.0f;
        float ar = (i < a->len) ? a->buf[i*2+1] / 32767.0f : 0.0f;
        float bl = (i < b->len) ? b->buf[i*2]   / 32767.0f : 0.0f;
        float br = (i < b->len) ? b->buf[i*2+1] / 32767.0f : 0.0f;
        buf_write(&out, i, al + bl, ar + br);
    }
    return out;
}

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    int frames = len / (NUM_CHANNELS * sizeof(int16_t));
    int16_t *out = (int16_t *)stream;
    memset(out, 0, len);

    SDL_LockMutex(g_mutex);
    for (int ch = 0; ch < MAX_MIX; ch++) {
        Voice *v = &g_voices[ch];
        if (!v->active || !v->snd || !v->snd->buf) continue;
        for (int i = 0; i < frames && v->pos < v->snd->len; i++, v->pos++) {
            float sl = v->snd->buf[v->pos * 2]     / 32767.0f * v->lgain;
            float sr = v->snd->buf[v->pos * 2 + 1] / 32767.0f * v->rgain;
            float ol = out[i * 2]     / 32767.0f + sl;
            float or_ = out[i * 2 + 1] / 32767.0f + sr;
            out[i * 2]     = (int16_t)(fclampf(ol,  -1.0f, 1.0f) * 32767.0f);
            out[i * 2 + 1] = (int16_t)(fclampf(or_, -1.0f, 1.0f) * 32767.0f);
        }
        if (v->pos >= v->snd->len) v->active = 0;
    }
    SDL_UnlockMutex(g_mutex);
}

/* Compute per-channel gains from source->listener geometry. Exposed
 * (audio.h) as audio_spatial_gains so it's unit-testable without a real
 * audio device -- the actual math this function runs, unchanged from
 * SHANKPIT's own static version. */
void audio_spatial_gains(float sx, float sy, float sz,
                          float lx, float ly, float lz, float lyaw,
                          float *out_l, float *out_r) {
    float dx = sx - lx;
    float dy = sy - ly;
    float dz = sz - lz;
    float dist2 = dx*dx + dy*dy + dz*dz;

    float vol = 1.0f / (1.0f + dist2 * 0.0001f);
    if (vol > 1.0f) vol = 1.0f;

    float angle_src = atan2f(dx, dz);
    float rel = angle_src - lyaw;
    float pan = sinf(rel);

    *out_l = vol * (1.0f - (pan > 0.0f ? pan : 0.0f));
    *out_r = vol * (1.0f - (pan < 0.0f ? -pan : 0.0f));
}

static void play_sound(int snd_id, float lgain, float rgain) {
    if (snd_id < 0 || snd_id >= MAX_SOUNDS) return;
    if (!g_snd[snd_id].buf) return;
    if (!g_mutex) return;

    SDL_LockMutex(g_mutex);
    int slot = -1;
    int best_pos = -1;
    for (int i = 0; i < MAX_MIX; i++) {
        if (!g_voices[i].active) { slot = i; break; }
        if (g_voices[i].pos > best_pos) { best_pos = g_voices[i].pos; slot = i; }
    }
    g_voices[slot].snd    = &g_snd[snd_id];
    g_voices[slot].pos    = 0;
    g_voices[slot].lgain  = lgain;
    g_voices[slot].rgain  = rgain;
    g_voices[slot].active = 1;
    SDL_UnlockMutex(g_mutex);
}

/* 0=WPN_KNIFE 1=WPN_MAGNUM 2=WPN_AR 3=WPN_SHOTGUN 4=WPN_SNIPER 5=WPN_KATANA
 * (matches shankpit-460's own packages/common/protocol.h WPN_* values
 * exactly -- confirmed, not assumed, before wiring call sites to pass
 * p->current_weapon straight through with no remapping table needed).
 * 6-10 = pentatonic footstep notes (C4 D4 E4 G4 A4). */
static const float PENTATONIC_HZ[5] = { 261.63f, 293.66f, 329.63f, 392.00f, 440.00f };

static void synth_play_weapon(SoundEngine *self, int weapon_idx,
                               float sx, float sy, float sz,
                               float lx, float ly, float lz, float lyaw) {
    (void)self;
    if (!g_dev || !g_mutex) return;
    if (weapon_idx < 0 || weapon_idx > 5) return;
    float lgain, rgain;
    audio_spatial_gains(sx, sy, sz, lx, ly, lz, lyaw, &lgain, &rgain);
    play_sound(weapon_idx, lgain, rgain);
}

static void synth_play_footstep(SoundEngine *self,
                                 float sx, float sy, float sz,
                                 float lx, float ly, float lz, float lyaw,
                                 int step_index) {
    (void)self;
    if (!g_dev || !g_mutex) return;
    int note = ((step_index % 5) + 5) % 5;
    float lgain, rgain;
    audio_spatial_gains(sx, sy, sz, lx, ly, lz, lyaw, &lgain, &rgain);
    play_sound(6 + note, lgain * 0.35f, rgain * 0.35f);
}

static void synth_play_ambient(SoundEngine *self, int ambient_id,
                                float sx, float sy, float sz,
                                float lx, float ly, float lz, float lyaw) {
    (void)self; (void)ambient_id; (void)sx; (void)sy; (void)sz;
    (void)lx; (void)ly; (void)lz; (void)lyaw;
    /* No ambient sound content designed yet -- see audio.h's own doc
     * comment on SoundEngine.play_ambient. Real, callable no-op: call
     * sites can wire this in today and it starts doing something the
     * moment ambient content exists, without another round of call-site
     * changes. */
}

static void synth_shutdown(SoundEngine *self) {
    (void)self;
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    if (g_mutex) {
        SDL_DestroyMutex(g_mutex);
        g_mutex = NULL;
    }
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (g_snd[i].buf) {
            free(g_snd[i].buf);
            g_snd[i].buf = NULL;
        }
    }
}

static SoundEngine g_synth_engine = {
    .play_weapon   = synth_play_weapon,
    .play_footstep = synth_play_footstep,
    .play_ambient  = synth_play_ambient,
    .shutdown      = synth_shutdown,
};

SoundEngine *audio_synth_create(void) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        SDL_Log("[audio] SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        return &g_synth_engine;
    }

    g_mutex = SDL_CreateMutex();
    if (!g_mutex) { SDL_Log("[audio] mutex create failed"); return &g_synth_engine; }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = NUM_CHANNELS;
    want.samples  = BUFFER_FRAMES;
    want.callback = audio_callback;

    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) {
        SDL_Log("[audio] SDL_OpenAudioDevice failed: %s -- audio disabled, gameplay unaffected", SDL_GetError());
        return &g_synth_engine;
    }

    /* 0: Knife */
    { SoundBuf tone = synth_tone(1600.0f, 0.035f, 0.5f, 40.0f);
      SoundBuf hat  = synth_noise(0.028f, 0.4f, 60.0f);
      SoundBuf mix  = synth_layer(&tone, &hat);
      free(tone.buf); free(hat.buf); g_snd[0] = mix; }
    /* 1: Magnum */
    g_snd[1] = synth_kick(200.0f, 38.0f, 0.22f, 0.95f);
    /* 2: AR */
    g_snd[2] = synth_noise(0.022f, 0.65f, 80.0f);
    /* 3: Shotgun */
    { SoundBuf kick  = synth_kick(175.0f, 52.0f, 0.14f, 0.85f);
      SoundBuf blast = synth_noise(0.12f, 0.75f, 18.0f);
      SoundBuf mix   = synth_layer(&kick, &blast);
      free(kick.buf); free(blast.buf); g_snd[3] = mix; }
    /* 4: Sniper */
    g_snd[4] = synth_kick(85.0f, 16.0f, 0.50f, 0.92f);
    /* 5: Katana */
    { SoundBuf tone = synth_tone(880.0f, 0.07f, 0.55f, 30.0f);
      SoundBuf hat  = synth_noise(0.065f, 0.5f, 35.0f);
      SoundBuf mix  = synth_layer(&tone, &hat);
      free(tone.buf); free(hat.buf); g_snd[5] = mix; }
    /* 6-10: pentatonic footstep notes */
    for (int n = 0; n < 5; n++) {
        g_snd[6 + n] = synth_tone(PENTATONIC_HZ[n], 0.10f, 0.38f, 22.0f);
    }

    SDL_PauseAudioDevice(g_dev, 0);
    SDL_Log("[audio] initialized: %d Hz stereo, %d sound buffers", SAMPLE_RATE, MAX_SOUNDS);
    return &g_synth_engine;
}
