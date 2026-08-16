/* audio.c -- the dispatch layer. Holds whichever SoundEngine is currently
 * active and routes the plain top-level calls (audio_play_weapon etc.)
 * through its vtable. This indirection is the whole point of NORTHSTAR
 * §7's interface requirement: gameplay call sites never reference a
 * specific backend, so swapping the synth engine for a future real-asset
 * engine is one audio_set_engine() call, not a find-and-replace across
 * every place a sound gets played.
 */
#include "audio.h"
#include <stddef.h>

static SoundEngine *g_active = NULL;

void audio_set_engine(SoundEngine *engine) {
    g_active = engine;
}

void audio_init(void) {
    audio_set_engine(audio_synth_create());
}

void audio_shutdown(void) {
    if (g_active && g_active->shutdown) {
        g_active->shutdown(g_active);
    }
    g_active = NULL;
}

void audio_play_weapon(int weapon_idx,
                        float sx, float sy, float sz,
                        float lx, float ly, float lz, float lyaw) {
    if (!g_active || !g_active->play_weapon) return;
    g_active->play_weapon(g_active, weapon_idx, sx, sy, sz, lx, ly, lz, lyaw);
}

void audio_play_footstep(float sx, float sy, float sz,
                          float lx, float ly, float lz, float lyaw,
                          int step_index) {
    if (!g_active || !g_active->play_footstep) return;
    g_active->play_footstep(g_active, sx, sy, sz, lx, ly, lz, lyaw, step_index);
}

void audio_play_ambient(int ambient_id,
                         float sx, float sy, float sz,
                         float lx, float ly, float lz, float lyaw) {
    if (!g_active || !g_active->play_ambient) return;
    g_active->play_ambient(g_active, ambient_id, sx, sy, sz, lx, ly, lz, lyaw);
}
