#include "proc_tex.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void proctex_init(void) {
    /* deterministic generators do not require runtime init */
}

static float fracf(float v) {
    return v - floorf(v);
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static float smoothstep01(float t) {
    return t * t * (3.0f - 2.0f * t);
}

static uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static float hash_2d(int x, int y, int seed) {
    uint32_t h = hash_u32((uint32_t)(x * 374761393 + y * 668265263 + seed * 362437));
    return (h & 0x00ffffffU) / 16777215.0f;
}

static float value_noise_2d(float x, float y, int seed) {
    int xi = (int)floorf(x);
    int yi = (int)floorf(y);
    float tx = smoothstep01(fracf(x));
    float ty = smoothstep01(fracf(y));

    float n00 = hash_2d(xi, yi, seed);
    float n10 = hash_2d(xi + 1, yi, seed);
    float n01 = hash_2d(xi, yi + 1, seed);
    float n11 = hash_2d(xi + 1, yi + 1, seed);

    float nx0 = lerpf(n00, n10, tx);
    float nx1 = lerpf(n01, n11, tx);
    return lerpf(nx0, nx1, ty);
}

static float fbm_2d(float x, float y, int seed) {
    float sum = 0.0f;
    float amp = 0.5f;
    float freq = 1.0f;
    for (int i = 0; i < 4; ++i) {
        sum += amp * value_noise_2d(x * freq, y * freq, seed + i * 67);
        freq *= 2.0f;
        amp *= 0.5f;
    }
    return sum;
}

int proc_tex_create(ProcTexture *t, int w, int h) {
    if (!t || w <= 0 || h <= 0) return 0;
    memset(t, 0, sizeof(*t));
    t->pixels = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    if (!t->pixels) return 0;

    t->width = w;
    t->height = h;
    glGenTextures(1, &t->tex_id);
    if (t->tex_id == 0) {
        free(t->pixels);
        memset(t, 0, sizeof(*t));
        return 0;
    }

    glBindTexture(GL_TEXTURE_2D, t->tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    return 1;
}

void proc_tex_destroy(ProcTexture *t) {
    if (!t) return;
    if (t->tex_id != 0) {
        glDeleteTextures(1, &t->tex_id);
    }
    free(t->pixels);
    memset(t, 0, sizeof(*t));
}

void proc_tex_upload(ProcTexture *t) {
    if (!t || t->tex_id == 0 || !t->pixels) return;
    glBindTexture(GL_TEXTURE_2D, t->tex_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->width, t->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void proctex_upload_to_gl(ProcTexture *t) {
    proc_tex_upload(t);
}

void proctex_make_noise_rgba(ProcTexture *t, int w, int h, uint32_t seed) {
    if (!t || !t->pixels || t->width != w || t->height != h) return;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            float u = (float)x / (float)w;
            float v = (float)y / (float)h;
            float grain = value_noise_2d(u * 26.0f, v * 26.0f, (int)seed);
            float scan = 0.9f + 0.1f * sinf(v * 130.0f + (float)(seed & 63U));
            float base = (0.45f + 0.55f * grain) * scan;
            if (base > 1.0f) base = 1.0f;
            unsigned char c = (unsigned char)(base * 255.0f);
            t->pixels[idx + 0] = c;
            t->pixels[idx + 1] = c;
            t->pixels[idx + 2] = c;
            t->pixels[idx + 3] = 255;
        }
    }
}

void proctex_make_glitch_marks_rgba(ProcTexture *t, int w, int h, uint32_t seed) {
    if (!t || !t->pixels || t->width != w || t->height != h) return;
    memset(t->pixels, 0, (size_t)w * (size_t)h * 4u);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float n = value_noise_2d((float)x * 0.24f, (float)y * 0.24f, (int)(seed + 911));
            float diag = fracf((float)x * 0.07f + (float)y * 0.11f + (float)(seed & 255U) * 0.001f);
            if (n > 0.82f && diag > 0.35f && diag < 0.52f) {
                size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 4u;
                t->pixels[idx + 0] = 240;
                t->pixels[idx + 1] = 30;
                t->pixels[idx + 2] = 220;
                t->pixels[idx + 3] = 100;
            }
        }
    }
}

void proc_tex_fill_emily_vibe(ProcTexture *t, float seed, float t_sec) {
    if (!t || !t->pixels) return;

    const float drift_x = t_sec * 0.05f;
    const float drift_y = t_sec * 0.03f;
    const int iseed = (int)(seed * 4096.0f);

    for (int y = 0; y < t->height; ++y) {
        for (int x = 0; x < t->width; ++x) {
            size_t idx = ((size_t)y * (size_t)t->width + (size_t)x) * 4u;
            float u = (float)x / (float)t->width;
            float v = (float)y / (float)t->height;

            float fog = fbm_2d(u * 2.5f + drift_x, v * 2.5f + drift_y, iseed);
            float grain = value_noise_2d(u * 28.0f + drift_x * 5.0f, v * 28.0f + drift_y * 4.0f, iseed + 1009);

            float stripe_n = value_noise_2d((u + v) * 16.0f + t_sec * 0.1f, (v - u) * 8.0f, iseed + 2203);
            float diag = fracf((u * 11.0f + v * 15.0f) + t_sec * 0.08f);
            float glitch = (stripe_n > 0.74f && diag > 0.35f && diag < 0.6f) ? 1.0f : 0.0f;

            float r = 0.05f + 0.10f * fog + 0.05f * grain;
            float g = 0.22f + 0.45f * fog + 0.08f * grain;
            float b = 0.28f + 0.52f * fog + 0.08f * grain;

            r += 0.30f * glitch;
            g += 0.02f * glitch;
            b += 0.22f * glitch;

            if (r > 1.0f) r = 1.0f;
            if (g > 1.0f) g = 1.0f;
            if (b > 1.0f) b = 1.0f;

            t->pixels[idx + 0] = (unsigned char)(r * 255.0f);
            t->pixels[idx + 1] = (unsigned char)(g * 255.0f);
            t->pixels[idx + 2] = (unsigned char)(b * 255.0f);
            t->pixels[idx + 3] = 255;
        }
    }
}
