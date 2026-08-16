#ifndef AUDIO_H
#define AUDIO_H

/* Spatial audio for shankpit-460 -- S169-09 (EMILY/BACKLOG.md), backported
 * from SHANKPIT's packages/audio/audio.c per docs2/NORTHSTAR.md §7's own
 * design record. That doc's own framing: this is a port, not a fresh
 * build -- the synthesis engine and spatial math below are carried over
 * as-is; what's actually new here (and what NORTHSTAR §7 explicitly asks
 * for before porting) is the SoundEngine interface, so a future real-
 * asset backend (recorded WAV/OGG via SDL_mixer or similar) can be a
 * second implementation behind the same call sites, not a rewrite of
 * every place that plays a sound.
 *
 * Call sites use the plain top-level functions below (audio_play_weapon
 * etc.) exactly like SHANKPIT's own free-function API -- the vtable
 * indirection lives inside audio.c's dispatch layer, invisible to
 * callers. Swapping backends means calling audio_set_engine() once, not
 * touching gameplay code.
 */

/* SoundEngine is the seam NORTHSTAR §7 asks for: a struct-of-function-
 * pointers backend. audio_synth_create() below is the only constructor
 * that exists today (the procedural PCM engine, ported verbatim); a
 * future recorded-asset backend would provide its own constructor
 * returning a SoundEngine of the same shape. */
typedef struct SoundEngine {
    void (*play_weapon)(struct SoundEngine *self, int weapon_idx,
                         float sx, float sy, float sz,
                         float lx, float ly, float lz, float lyaw);
    void (*play_footstep)(struct SoundEngine *self,
                           float sx, float sy, float sz,
                           float lx, float ly, float lz, float lyaw,
                           int step_index);
    /* play_ambient did not exist in SHANKPIT's original audio.c -- added
     * here because NORTHSTAR §7 names it as part of the target interface
     * shape. No ambient sound content has been designed yet (matches the
     * same "flag the gap, don't invent content" discipline the rest of
     * this port follows) -- audio_synth_create()'s implementation of this
     * hook is a real, callable no-op today, not missing from the vtable,
     * so call sites can start using it now and it starts doing something
     * the moment ambient sound content actually gets designed. */
    void (*play_ambient)(struct SoundEngine *self, int ambient_id,
                          float sx, float sy, float sz,
                          float lx, float ly, float lz, float lyaw);
    void (*shutdown)(struct SoundEngine *self);
} SoundEngine;

/* audio_synth_create returns the procedural-PCM SoundEngine -- the
 * verbatim port of SHANKPIT's synthesis backend (MIDI drum/bass
 * archetypes for weapons, pentatonic footstep notes, sin(angle) spatial
 * panning, soft distance rolloff). Ownership: caller must eventually pass
 * the returned engine to its own shutdown function (or via audio_shutdown
 * if set active through audio_set_engine). */
SoundEngine *audio_synth_create(void);

/* audio_init creates the default (synth) engine and makes it active --
 * equivalent to audio_set_engine(audio_synth_create()). This is what
 * every call site should use unless it specifically wants a non-default
 * backend. */
void audio_init(void);
void audio_shutdown(void);

/* audio_set_engine swaps the active engine. Does NOT shut down whatever
 * engine was previously active -- callers that are replacing an engine
 * (not doing first-time setup) are responsible for calling the old
 * engine's own shutdown first if they want it torn down. */
void audio_set_engine(SoundEngine *engine);

/* Dispatch functions -- route through whichever engine is currently
 * active. Safe to call with no engine set (audio_init not yet called, or
 * audio_init itself failed to open a device): no-ops, matching the
 * original audio.c's own "if (!g_dev) return" fail-safe. */
void audio_play_weapon(int weapon_idx,
                        float sx, float sy, float sz,
                        float lx, float ly, float lz, float lyaw);
void audio_play_footstep(float sx, float sy, float sz,
                          float lx, float ly, float lz, float lyaw,
                          int step_index);
void audio_play_ambient(int ambient_id,
                         float sx, float sy, float sz,
                         float lx, float ly, float lz, float lyaw);

/* spatial_gains is exposed (unlike SHANKPIT's original, which kept it
 * static) so it can be unit-tested directly -- the geometry math is the
 * one part of this module that's meaningfully testable without a real
 * audio device. Same signature/semantics as SHANKPIT's own static
 * version. */
void audio_spatial_gains(float sx, float sy, float sz,
                          float lx, float ly, float lz, float lyaw,
                          float *out_l, float *out_r);

#endif /* AUDIO_H */
