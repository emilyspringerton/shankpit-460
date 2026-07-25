/* NOT THE REAL SERVER (S155-02, EMILY/BACKLOG.md): this implementation is fully superseded by
 * apps/server/src/main.c and is not built, run, or referenced by any script, Makefile, or
 * systemd unit in this repo. It exists only as unremoved history from before the real server
 * consolidated. Do not extend or wire this up without first checking apps/server/src/main.c --
 * a real engineer/agent lost real time re-discovering this the hard way once already. This file
 * is kept pending a deliberate NORTHSTAR scoping pass on what specifically gets cut vs. kept
 * (see this repo's own CLAUDE.md), not because it's still in use. */
#define _WIN32_WINNT 0x0600
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h> 

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <arpa/inet.h>
#endif

void sleep_ms(int ms) {
    #ifdef _WIN32
    Sleep(ms);
    #else
    usleep(ms * 1000);
    #endif
}

#include "heartbeat.h"
#include "../../../packages/protocol/protocol.h"
#include "../../../packages/map/map.h"
#include "../../../packages/map/map.c"

ServerState state;
GameMap level;
struct sockaddr_in clients[MAX_CLIENTS];
int client_connected[MAX_CLIENTS] = {0};
int demo_mode = 0; // NEW: Bot Flag

#define HISTORY_SIZE 64
typedef struct { int valid; Vec3 pos[MAX_CLIENTS]; } WorldSnapshot;
WorldSnapshot history[HISTORY_SIZE];
Vec3 backup_pos[MAX_CLIENTS];

float rand_f() { return ((float)(rand()%1000)/500.0f) - 1.0f; }
Vec3 get_aim_vector(float yaw, float pitch, float spread) {
    float ry = yaw * (M_PI/180.0f); float rp = pitch * (M_PI/180.0f);
    Vec3 fwd = { sin(ry)*cos(rp), sin(rp), cos(ry)*cos(rp) };
    if (spread > 0) { fwd.x+=rand_f()*spread; fwd.y+=rand_f()*spread; fwd.z+=rand_f()*spread; }
    return fwd;
}
int ray_hit_sphere(Vec3 origin, Vec3 dir, Vec3 target, float radius) {
    Vec3 oc = { target.x-origin.x, target.y-origin.y, target.z-origin.z };
    float t = (oc.x*dir.x + oc.y*dir.y + oc.z*dir.z);
    if (t < 0) return 0;
    Vec3 p = { origin.x + dir.x*t, origin.y + dir.y*t, origin.z + dir.z*t };
    return (powf(p.x-target.x, 2) + powf(p.y-target.y, 2) + powf(p.z-target.z, 2)) < (radius*radius);
}
void record_snapshot() {
    int idx = state.server_tick % HISTORY_SIZE; history[idx].valid = 1;
    for(int i=0; i<MAX_CLIENTS; i++) history[idx].pos[i] = state.players[i].pos;
}
void rewind_world(uint32_t target_tick) {
    int age = state.server_tick - target_tick;
    if (age < 0 || age >= HISTORY_SIZE) return;
    int idx = target_tick % HISTORY_SIZE;
    if (!history[idx].valid) return;
    for(int i=0; i<MAX_CLIENTS; i++) { backup_pos[i] = state.players[i].pos; state.players[i].pos = history[idx].pos[i]; }
}
void restore_world() {
    for(int i=0; i<MAX_CLIENTS; i++) state.players[i].pos = backup_pos[i];
}

void spawn_grenade(int pid) {
    PlayerState *p = &state.players[pid];
    if (p->grenades <= 0) return;
    p->grenades--;
    int gid = -1; for(int i=0; i<MAX_GRENADES; i++) if(!state.grenades[i].active) { gid=i; break; }
    if (gid == -1) return;
    GrenadeState *g = &state.grenades[gid];
    g->active = 1; g->owner_id = pid; g->timer = 120;
    g->pos = p->pos; g->pos.y += 1.5f;
    Vec3 aim = get_aim_vector(p->yaw, p->pitch, 0);
    g->vel.x = aim.x * 0.8f + p->vel.x; g->vel.y = aim.y * 0.8f + 0.2f; g->vel.z = aim.z * 0.8f + p->vel.z;
}

void update_grenades() {
    for(int i=0; i<MAX_GRENADES; i++) {
        GrenadeState *g = &state.grenades[i];
        if (!g->active) continue;
        g->pos.x += g->vel.x; g->pos.y += g->vel.y; g->pos.z += g->vel.z;
        g->vel.y -= 0.02f;
        if (g->pos.y < 0) { g->pos.y = 0; g->vel.y *= -0.6f; g->vel.x *= 0.8f; g->vel.z *= 0.8f; }
        if (map_check_point(&level, g->pos.x, g->pos.y, g->pos.z)) { g->vel.x *= -0.5f; g->vel.z *= -0.5f; }
        g->timer--;
        if (g->timer <= 0) {
            g->active = 0;
            for(int p=0; p<MAX_CLIENTS; p++) {
                if(!state.players[p].active) continue;
                float d = sqrtf(powf(state.players[p].pos.x - g->pos.x, 2) + powf(state.players[p].pos.z - g->pos.z, 2));
                if (d < 5.0f) {
                    state.players[p].health -= (int)(100 * (1.0f - d/5.0f));
                    state.players[p].vel.y += 0.5f;
                    if(state.players[p].health <= 0) {
                        state.players[p].deaths++; state.players[g->owner_id].kills++;
                        state.players[p].health = 100; state.players[p].shield = 100;
                        state.players[p].pos.x = rand_f()*20; state.players[p].pos.z = rand_f()*20;
                    }
                }
            }
        }
    }
}

void process_shoot(int pid, uint32_t render_tick) {
    PlayerState *p = &state.players[pid];
    int w = p->current_weapon;
    if (p->attack_cooldown > 0) return;
    if (w != WPN_KNIFE && p->ammo[w] <= 0) { p->attack_cooldown = 60; p->ammo[w] = WPN_STATS[w].ammo_max; return; }
    
    rewind_world(render_tick);
    if(w != WPN_KNIFE) p->ammo[w]--;
    p->attack_cooldown = WPN_STATS[w].rof;
    p->is_shooting = 5;
    
    int pellets = WPN_STATS[w].cnt; float range = (w == WPN_KNIFE) ? 2.5f : 200.0f;
    Vec3 origin = { p->pos.x, p->pos.y + 1.5f, p->pos.z };

    for (int k=0; k<pellets; k++) {
        Vec3 dir = get_aim_vector(p->yaw, p->pitch, WPN_STATS[w].spr);
        if (map_ray_cast(&level, origin, dir, range)) continue;
        for(int i=0; i<MAX_CLIENTS; i++) {
            if (i == pid || !state.players[i].active) continue;
            PlayerState *vic = &state.players[i];
            float dist = sqrtf(powf(vic->pos.x - p->pos.x, 2) + powf(vic->pos.z - p->pos.z, 2));
            if (dist > range) continue;
            if (ray_hit_sphere(origin, dir, vic->pos, 1.0f)) {
                p->hit_feedback = (vic->shield > 0) ? 1 : 2;
                int dmg = WPN_STATS[w].dmg;
                if (vic->shield > 0) { vic->shield -= dmg; if(vic->shield < 0) { vic->health += vic->shield; vic->shield = 0; } } 
                else { vic->health -= dmg; }
                if (vic->health <= 0) { 
                    vic->health = 100; vic->shield = 100; vic->pos.x = rand_f()*20; vic->pos.z = rand_f()*20;
                    vic->deaths++; p->kills++;
                    for(int j=0; j<MAX_WEAPONS; j++) vic->ammo[j] = WPN_STATS[j].ammo_max;
                }
            }
        }
    }
    restore_world();
}

int main(int argc, char* argv[]) {
    #ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    #endif

    // ARG PARSING
    for(int i=1; i<argc; i++) {
        if(strcmp(argv[i], "--demo") == 0) {
            demo_mode = 1;
            printf("[SERVER] Demo Mode Active: Bots Enabled\n");
        }
    }

    map_init(&level);
    for(int i=0; i<MAX_VEHICLES; i++) { state.vehicles[i].id = i; state.vehicles[i].pos.x = i * 10.0f; state.vehicles[i].pos.z = 10.0f; state.vehicles[i].driver_id = -1; }
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(PORT); addr.sin_addr.s_addr = INADDR_ANY;
    bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    
    #ifdef _WIN32
        DWORD timeout = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    #else
        struct timeval tv = {0, 1000};
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    #endif
    
    state.match_timer = MATCH_DURATION;
    time_t last_heartbeat = 0;
    
    printf("SHANK SERVER ONLINE (FIXED) :%d\n", PORT);
    struct sockaddr_in cli; socklen_t len = sizeof(cli); char buf[1024];

    while(1) {
        time_t now = time(NULL);
        if (now - last_heartbeat > 5) {
            int active_count = 0;
            for(int i=0; i<MAX_CLIENTS; i++) if(state.players[i].active) active_count++;
            // Only announce if not demo mode (Keep demo local)
            if (!demo_mode) {
                send_heartbeat_to_master("127.0.0.1", 8080, PORT, "ShankArena-1", active_count, MAX_CLIENTS);
            }
            last_heartbeat = now;
        }

        state.server_tick++;
        state.match_timer--;
        if (state.match_timer <= 0) {
            state.match_timer = MATCH_DURATION;
            for(int i=0; i<MAX_CLIENTS; i++) { state.players[i].kills = 0; state.players[i].deaths = 0; state.players[i].health = 100; }
        }

        // --- BOT LOGIC ---
        if (demo_mode) {
            int bot_id = 7; // Reserved ID
            if (!state.players[bot_id].active) {
                state.players[bot_id].active = 1;
                state.players[bot_id].health = 100;
                state.players[bot_id].pos.z = 10.0f;
            }
            // Strafe left/right
            state.players[bot_id].vel.x = sinf(state.server_tick * 0.05f) * 0.2f;
            state.players[bot_id].yaw += 1.0f; // Spin
            
            if (state.players[bot_id].health <= 0) {
                state.players[bot_id].health = 100;
                state.players[bot_id].pos.x = 0;
                state.players[bot_id].pos.z = 10.0f;
            }
        }

        int n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr*)&cli, &len);
        if (n > 0) {
            ClientPacket *pkt = (ClientPacket*)buf;
            int pid = 0; 
            if (!state.players[pid].active) {
                state.players[pid].active = 1; state.players[pid].health = 100; state.players[pid].shield = 100; state.players[pid].grenades = 3;
                client_connected[pid] = 1; clients[pid] = cli;
            }
            PlayerState *p = &state.players[pid];
            float r = pkt->yaw * (M_PI/180.0f);
            if (p->vehicle_id == -1) { p->vel.x += (pkt->fwd*cos(r) - pkt->strafe*sin(r)) * 0.05f; p->vel.z += (pkt->fwd*sin(r) + pkt->strafe*cos(r)) * 0.05f; }
            p->yaw = pkt->yaw; p->pitch = pkt->pitch;
            if (pkt->weapon_req >= 0) p->current_weapon = pkt->weapon_req;
            if (pkt->shoot) process_shoot(pid, pkt->render_tick);
            if (pkt->throw_grenade) spawn_grenade(pid);
            if (pkt->interact) {
                if (p->vehicle_id == -1) {
                    for(int v=0; v<MAX_VEHICLES; v++) {
                        float d = sqrtf(powf(state.vehicles[v].pos.x - p->pos.x, 2) + powf(state.vehicles[v].pos.z - p->pos.z, 2));
                        if (d < 3.0f && state.vehicles[v].driver_id == -1) {
                            state.vehicles[v].driver_id = pid; p->vehicle_id = v; p->pos = state.vehicles[v].pos;
                            break;
                        }
                    }
                } else {
                    state.vehicles[p->vehicle_id].driver_id = -1; p->vehicle_id = -1; p->pos.y += 2.0f;
                }
            }
        }

        update_grenades();
        for(int i=0; i<MAX_VEHICLES; i++) if(state.vehicles[i].driver_id != -1) state.vehicles[i].pos = state.players[state.vehicles[i].driver_id].pos;

        for(int i=0; i<MAX_CLIENTS; i++) {
            if(!state.players[i].active) continue;
            PlayerState *p = &state.players[i];
            if(p->vehicle_id == -1) { p->pos.x += p->vel.x; p->pos.z += p->vel.z; map_resolve_collision(&level, &p->pos, &p->vel); p->vel.x *= 0.9f; p->vel.z *= 0.9f; }
            else { p->pos = state.vehicles[p->vehicle_id].pos; }
            if (p->attack_cooldown > 0) p->attack_cooldown--;
            if (p->is_shooting > 0) p->is_shooting--;
            p->hit_feedback = 0;
        }
        record_snapshot();
        for(int i=0; i<MAX_CLIENTS; i++) if(client_connected[i]) sendto(sockfd, (const char*)&state, sizeof(state), 0, (struct sockaddr*)&clients[i], sizeof(clients[i]));
        sleep_ms(16);
    }
}
