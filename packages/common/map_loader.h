#ifndef MAP_LOADER_H
#define MAP_LOADER_H
// map_loader.h — bare-bones external map format (v0).
//
// This is deliberately the smallest possible evolution of the existing map
// system: geometry used to live only as hardcoded Box arrays inside
// physics.h (map_geo_stadium / map_geo_garage / map_geo_voxworld). This
// loader lets a map be authored as a plain-text file instead, which is the
// single shared prerequisite for a level editor and AI map generation
// (neither can target C source literals). See docs2/maps-report.md.
//
// FORMAT (plain text, one entry per line, '#' starts a comment):
//
//   box   <x> <y> <z> <w> <h> <d>     axis-aligned box, center + full size
//   spawn <x> <y> <z>                 player spawn point (round-robin by slot)
//   poi   <name> <x> <y> <z>          named point of interest (e.g. the
//                                     rocket_launcher spawn). Parsed and
//                                     stored only — no pickup system exists
//                                     in the live game yet.
//
// CONVENTIONS (inherited from the live engine, do not silently break):
//   - The FIRST box line must be the floor slab. Every geometry consumer in
//     physics.h (resolve_collision, trace_map_boxes, draw_map in the client)
//     iterates from index 1 and skips index 0; ground contact is the y<0
//     check, not box 0.
//   - Loaded maps are served under SCENE_STADIUM, so geometry must fit the
//     stadium envelope: |x|,|z| within STADIUM_BOUNDS (420) and above
//     STADIUM_KILL_Y (-80), or players clipping out will be force-respawned.
//
// Loading: map_load_default() reads $SHANKPIT_MAP if set, else
// "maps/v0_shankpit.map" (cwd-relative). On any failure it leaves
// map_loader_active = 0 and the engine falls back to the hardcoded stadium —
// a client/server missing the file keeps working exactly as before, but a
// mismatched pair will disagree about geometry, so ship the map file with
// both (release.yml copies maps/ into both bundles).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z, w, h, d; } Box;

#define MAP_LOADED_MAX_BOXES 256
#define MAP_LOADED_MAX_SPAWNS 32
#define MAP_LOADED_MAX_POIS 16
#define MAP_POI_NAME_LEN 32

typedef struct { float x, y, z; } MapSpawnPoint;
typedef struct { char name[MAP_POI_NAME_LEN]; float x, y, z; } MapPoi;

static Box map_geo_loaded[MAP_LOADED_MAX_BOXES];
static int map_geo_loaded_count = 0;
static MapSpawnPoint map_spawns_loaded[MAP_LOADED_MAX_SPAWNS];
static int map_spawns_loaded_count = 0;
static MapPoi map_pois_loaded[MAP_LOADED_MAX_POIS];
static int map_pois_loaded_count = 0;
static int map_loader_active = 0;
static char map_loaded_path[256] = "";

// Returns the number of boxes loaded (>0 on success), 0 on failure.
// On failure the previously loaded map (if any) is left untouched.
static inline int map_load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("MAP_LOAD: cannot open '%s'\n", path);
        return 0;
    }

    Box boxes[MAP_LOADED_MAX_BOXES];
    MapSpawnPoint spawns[MAP_LOADED_MAX_SPAWNS];
    MapPoi pois[MAP_LOADED_MAX_POIS];
    int n_boxes = 0, n_spawns = 0, n_pois = 0;
    int lineno = 0, bad = 0;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char keyword[16];
        if (sscanf(line, "%15s", keyword) != 1) continue; // blank line

        if (strcmp(keyword, "box") == 0) {
            Box b;
            if (sscanf(line, "box %f %f %f %f %f %f", &b.x, &b.y, &b.z, &b.w, &b.h, &b.d) == 6 &&
                n_boxes < MAP_LOADED_MAX_BOXES && b.w > 0.0f && b.h > 0.0f && b.d > 0.0f) {
                boxes[n_boxes++] = b;
            } else { bad++; printf("MAP_LOAD: %s:%d bad box line\n", path, lineno); }
        } else if (strcmp(keyword, "spawn") == 0) {
            MapSpawnPoint s;
            if (sscanf(line, "spawn %f %f %f", &s.x, &s.y, &s.z) == 3 &&
                n_spawns < MAP_LOADED_MAX_SPAWNS) {
                spawns[n_spawns++] = s;
            } else { bad++; printf("MAP_LOAD: %s:%d bad spawn line\n", path, lineno); }
        } else if (strcmp(keyword, "poi") == 0) {
            MapPoi p;
            if (sscanf(line, "poi %31s %f %f %f", p.name, &p.x, &p.y, &p.z) == 4 &&
                n_pois < MAP_LOADED_MAX_POIS) {
                pois[n_pois++] = p;
            } else { bad++; printf("MAP_LOAD: %s:%d bad poi line\n", path, lineno); }
        } else {
            bad++;
            printf("MAP_LOAD: %s:%d unknown keyword '%s'\n", path, lineno, keyword);
        }
    }
    fclose(f);

    // Need at least the floor (index 0, skipped by collision/render loops)
    // plus one real piece of geometry to be a playable map.
    if (n_boxes < 2 || bad > 0) {
        printf("MAP_LOAD: rejecting '%s' (%d boxes, %d bad lines) — keeping current map\n",
               path, n_boxes, bad);
        return 0;
    }

    memcpy(map_geo_loaded, boxes, sizeof(Box) * (size_t)n_boxes);
    map_geo_loaded_count = n_boxes;
    memcpy(map_spawns_loaded, spawns, sizeof(MapSpawnPoint) * (size_t)n_spawns);
    map_spawns_loaded_count = n_spawns;
    memcpy(map_pois_loaded, pois, sizeof(MapPoi) * (size_t)n_pois);
    map_pois_loaded_count = n_pois;
    map_loader_active = 1;
    snprintf(map_loaded_path, sizeof(map_loaded_path), "%s", path);
    printf("MAP_LOAD: '%s' loaded — %d boxes, %d spawns, %d pois\n",
           path, n_boxes, n_spawns, n_pois);
    return n_boxes;
}

// $SHANKPIT_MAP overrides; default is the shipped v0 map. Missing file is a
// soft fallback to the hardcoded stadium so old deployments keep working.
static inline void map_load_default(void) {
    const char *path = getenv("SHANKPIT_MAP");
    if (!path || !path[0]) path = "maps/v0_shankpit.map";
    if (!map_load_file(path)) {
        printf("MAP_LOAD: falling back to built-in stadium geometry\n");
    }
}

#endif
