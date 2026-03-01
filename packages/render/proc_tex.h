#ifndef PROC_TEX_H
#define PROC_TEX_H

#include <stddef.h>
#include <stdint.h>
#include <SDL2/SDL_opengl.h>

typedef struct ProcTexture {
    int width;
    int height;
    GLuint tex_id;
    unsigned char *pixels;
} ProcTexture;

void proctex_init(void);
int proc_tex_create(ProcTexture *t, int w, int h);
void proc_tex_destroy(ProcTexture *t);
void proc_tex_upload(ProcTexture *t);
void proc_tex_fill_emily_vibe(ProcTexture *t, float seed, float t_sec);
void proctex_make_noise_rgba(ProcTexture *t, int w, int h, uint32_t seed);
void proctex_make_glitch_marks_rgba(ProcTexture *t, int w, int h, uint32_t seed);
void proctex_upload_to_gl(ProcTexture *t);

#endif
