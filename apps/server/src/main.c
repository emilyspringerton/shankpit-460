// apps/server/src/main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

#include "../../../packages/common/protocol.h"
#include "../../../packages/common/physics.h"
#include "../../../packages/common/shared_movement.h"
#include "../../../packages/common/net_sim.h"
#include "../../../packages/common/hmac_sha256.h"
#include "../../../packages/common/http_client.h"
#include "../../../packages/simulation/local_game.h"
#include "server_mode.h"
#include "server_state.h"

int sock = -1;
struct sockaddr_in bind_addr;
unsigned int client_last_seq[MAX_CLIENTS];

// Server ticks at 16ms (see the usleep(16000) in main's loop) -> 62.5
// ticks/sec -> exactly 3750 ticks/minute.
#define TICKS_PER_MINUTE 3750
#define DEFAULT_MATCH_MINUTES 10

// Connect-ticket verification (S156-02, EMILY/BACKLOG.md SECTION 156).
// Wire format (36 bytes total, appended after the 12-byte NetHeader in a
// PACKET_CONNECT payload): player_id (16 raw UUID bytes) || expires_at
// (4-byte little-endian unix timestamp) || HMAC-SHA256(secret, the first
// 20 bytes) truncated to 16 bytes. Minted by IDUNA's ShankpitTicketHandler
// (internal/http/handlers/shankpit_ticket.go) — the two sides must agree
// on the SHANKPIT_TICKET_SECRET env var byte-for-byte (raw string bytes,
// not hex-decoded — see that handler's doc comment).
#define TICKET_PAYLOAD_LEN 20  // player_id(16) + expires_at(4)
#define TICKET_MAC_LEN 16      // truncated HMAC-SHA256
#define TICKET_TOTAL_LEN (TICKET_PAYLOAD_LEN + TICKET_MAC_LEN) // 36
static unsigned char ticket_secret[256];
static int ticket_secret_len = 0;

// IDUNA agent config (S156-04, EMILY/BACKLOG.md SECTION 156) — used to
// authenticate as the SHANKPIT460-SERVER M2M agent (config/agents.json)
// and report match results. Same env var names used across the rest of
// the monorepo (see CLAUDE.md's "Key Env Vars"). Missing config means
// reporting is silently skipped (soft warn) -- this is telemetry, not
// identity/security, so unlike ticket_secret it does NOT fail closed.
static char iduna_host[128] = "127.0.0.1";
static int iduna_port = 8080;
static char iduna_agent_name[128] = "";
static char iduna_agent_secret[256] = "";
static int iduna_agent_configured = 0;

typedef struct {
    int active;
    int welcomed;
    int cmd_seen;
    struct sockaddr_in addr;
    double last_heard;
    int player_id;
} ClientSlot;

static ClientSlot slots[MAX_CLIENTS];

typedef struct {
    int enabled;
    FILE *file;
    int target_id;
    float cam_x;
    float cam_y;
    float cam_z;
    float cam_yaw;
    float cam_pitch;
    float cam_zoom;
} RecorderState;

static RecorderState recorder = {0};

#define SERVER_SNAPSHOT_INTERVAL_TICKS 3

#define RECORDER_SHAKE_POS 0.08f
#define RECORDER_SHAKE_ANGLE 0.35f
#define RECORDER_SMOOTH_POS 0.08f
#define RECORDER_SMOOTH_ANGLE 0.18f
#define RECORDER_NORTH_X 0.0f
#define RECORDER_NORTH_Y 6.5f
#define RECORDER_NORTH_Z -32.0f

unsigned int get_server_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static double now_seconds(void) {
    return (double)get_server_time() / 1000.0;
}

static int addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int find_slot_by_addr(const struct sockaddr_in *addr) {
    for (int i = 1; i < MAX_CLIENTS; i++) {
        if (slots[i].active && addr_equal(&slots[i].addr, addr)) {
            return i;
        }
    }
    return -1;
}

// $SHANKPIT_CONNECT_SCENE=stadium drops new connections straight into the
// arena scene (the file-loaded map) instead of the garage hub — used by map
// playtests and headless bot-eval runs, where nothing presses USE at a
// portal. Default is unchanged: garage.
static int connect_scene(void) {
    const char *s = getenv("SHANKPIT_CONNECT_SCENE");
    if (s && strcmp(s, "stadium") == 0) return SCENE_STADIUM;
    return SCENE_GARAGE_OSAKA;
}

static int alloc_slot(const struct sockaddr_in *addr) {
    for (int i = 1; i < MAX_CLIENTS; i++) {
        if (!slots[i].active) {
            memset(&local_state.players[i], 0, sizeof(PlayerState));
            local_state.players[i].id = i;
            local_state.players[i].scene_id = connect_scene();
            local_state.players[i].active = 0;
            phys_respawn(&local_state.players[i], get_server_time());
            local_state.players[i].yaw = 0.0f;
            local_state.players[i].pitch = 0.0f;

            slots[i].active = 1;
            slots[i].welcomed = 0;
            slots[i].cmd_seen = 0;
            slots[i].addr = *addr;
            slots[i].last_heard = now_seconds();
            slots[i].player_id = i;

            local_state.clients[i] = *addr;
            local_state.client_meta[i].active = 0;
            local_state.client_meta[i].last_heard_ms = get_server_time();
            client_last_seq[i] = 0;

            return i;
        }
    }
    return -1;
}

static void free_slot(int slot) {
    if (slot <= 0 || slot >= MAX_CLIENTS) return;
    slots[slot].active = 0;
    slots[slot].welcomed = 0;
    slots[slot].cmd_seen = 0;
    memset(&slots[slot].addr, 0, sizeof(struct sockaddr_in));
    slots[slot].last_heard = 0.0;
    slots[slot].player_id = -1;
    server_disconnect(slot, client_last_seq);
}

static void send_welcome(const struct sockaddr_in *addr, int client_id) {
    unsigned int now = get_server_time();
    NetHeader h;
    h.type = PACKET_WELCOME;
    h.client_id = (unsigned char)client_id;
    h.sequence = 0;
    h.timestamp = now;
    h.entity_count = 0;
    h.scene_id = (unsigned char)local_state.players[client_id].scene_id;
    sendto(sock, (char*)&h, sizeof(NetHeader), 0,
           (const struct sockaddr*)addr, sizeof(struct sockaddr_in));
    if (client_id > 0 && client_id < MAX_CLIENTS) {
        slots[client_id].welcomed = 1;
    }
}

static int ensure_slot_for_sender(const struct sockaddr_in *sender) {
    int slot = find_slot_by_addr(sender);
    if (slot != -1) {
        slots[slot].last_heard = now_seconds();
        local_state.client_meta[slot].last_heard_ms = get_server_time();
        return slot;
    }

    int new_slot = alloc_slot(sender);
    if (new_slot != -1) {
        char ip_buf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &sender->sin_addr, ip_buf, sizeof(ip_buf));
        printf("CLIENT %d CONNECTED (%s:%d)\n", new_slot, ip_buf, ntohs(sender->sin_port));
        send_welcome(sender, new_slot);
    }
    return new_slot;
}

// find_slot_by_player_id returns the active slot already authenticated as
// the given real (IDUNA-issued) player_id, or -1 if none. Enforces
// one-seat-per-identity (VS2 hard constraint) — a second concurrent
// connect for an already-connected player_id is rejected, not migrated.
static int find_slot_by_player_id(const unsigned char player_id[16]) {
    for (int i = 1; i < MAX_CLIENTS; i++) {
        if (slots[i].active && local_state.players[i].has_player_id &&
            memcmp(local_state.players[i].player_id, player_id, 16) == 0) {
            return i;
        }
    }
    return -1;
}

// verify_connect_ticket checks the ticket appended to a PACKET_CONNECT
// payload (see TICKET_TOTAL_LEN's doc comment for the wire format). On
// success writes the 16-byte player_id to out_player_id and returns 1;
// otherwise returns 0 without writing anything. Fails closed: if the
// server has no secret configured at all, every ticket is rejected rather
// than silently accepting unauthenticated connects (see load_ticket_secret).
static int verify_connect_ticket(const char *buffer, int size, unsigned char out_player_id[16]) {
    if (ticket_secret_len == 0) return 0;
    if (size < (int)sizeof(NetHeader) + TICKET_TOTAL_LEN) return 0;

    const unsigned char *ticket = (const unsigned char *)(buffer + sizeof(NetHeader));
    const unsigned char *payload = ticket;            // player_id(16) + expires_at(4)
    const unsigned char *given_mac = ticket + TICKET_PAYLOAD_LEN;

    unsigned char expected_mac[32];
    hmac_sha256(ticket_secret, (size_t)ticket_secret_len, payload, TICKET_PAYLOAD_LEN, expected_mac);
    if (!hmac_sha256_verify(given_mac, expected_mac, TICKET_MAC_LEN)) {
        return 0;
    }

    unsigned int expires_at =
        (unsigned int)payload[16] | ((unsigned int)payload[17] << 8) |
        ((unsigned int)payload[18] << 16) | ((unsigned int)payload[19] << 24);
    if ((unsigned int)time(NULL) > expires_at) {
        return 0; // ticket expired
    }

    memcpy(out_player_id, payload, 16);
    return 1;
}

// load_ticket_secret reads SHANKPIT_TICKET_SECRET at startup. Deliberately
// no fallback/default: an unset secret means ticket_secret_len stays 0,
// which verify_connect_ticket treats as "reject everything" — fail closed,
// not fail open, if this gets deployed without the secret provisioned.
static void load_ticket_secret(void) {
    const char *env = getenv("SHANKPIT_TICKET_SECRET");
    if (!env || !env[0]) {
        printf("WARNING: SHANKPIT_TICKET_SECRET not set — all connect attempts will be rejected (fail closed, not fail open)\n");
        return;
    }
    size_t len = strlen(env);
    if (len > sizeof(ticket_secret)) len = sizeof(ticket_secret);
    memcpy(ticket_secret, env, len);
    ticket_secret_len = (int)len;
    printf("SHANKPIT_TICKET_SECRET loaded (%d bytes)\n", ticket_secret_len);
}

// load_iduna_agent_config reads IDUNA_BASE_URL / IDUNA_AGENT_NAME /
// IDUNA_AGENT_SECRET at startup. IDUNA_BASE_URL may be "host:port" or a
// full "http://host:port" URL (scheme and any trailing path are stripped);
// defaults to 127.0.0.1:8080 if unset.
static void load_iduna_agent_config(void) {
    const char *base_url = getenv("IDUNA_BASE_URL");
    if (base_url && base_url[0]) {
        const char *host_start = base_url;
        if (strncmp(host_start, "http://", 7) == 0) host_start += 7;
        else if (strncmp(host_start, "https://", 8) == 0) host_start += 8;

        char host_buf[128];
        strncpy(host_buf, host_start, sizeof(host_buf) - 1);
        host_buf[sizeof(host_buf) - 1] = '\0';
        char *slash = strchr(host_buf, '/');
        if (slash) *slash = '\0';

        char *colon = strchr(host_buf, ':');
        int port = iduna_port;
        if (colon) {
            port = atoi(colon + 1);
            *colon = '\0';
        }
        strncpy(iduna_host, host_buf, sizeof(iduna_host) - 1);
        iduna_host[sizeof(iduna_host) - 1] = '\0';
        if (port > 0) iduna_port = port;
    }

    const char *name = getenv("IDUNA_AGENT_NAME");
    const char *secret = getenv("IDUNA_AGENT_SECRET");
    if (name && name[0] && secret && secret[0]) {
        strncpy(iduna_agent_name, name, sizeof(iduna_agent_name) - 1);
        iduna_agent_name[sizeof(iduna_agent_name) - 1] = '\0';
        strncpy(iduna_agent_secret, secret, sizeof(iduna_agent_secret) - 1);
        iduna_agent_secret[sizeof(iduna_agent_secret) - 1] = '\0';
        iduna_agent_configured = 1;
        printf("IDUNA agent configured: name=%s host=%s:%d (match results will be reported)\n",
               iduna_agent_name, iduna_host, iduna_port);
    } else {
        printf("WARNING: IDUNA_AGENT_NAME/IDUNA_AGENT_SECRET not set -- match results will NOT be reported to IDUNA (S156-04)\n");
    }
}

// uuid_bytes_to_string formats 16 raw bytes as a standard 8-4-4-4-12
// hyphenated hex UUID string (out must be at least 37 bytes) — the form
// IDUNA's player endpoints expect in the URL path.
static void uuid_bytes_to_string(const unsigned char b[16], char out[37]) {
    snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

// report_match_results POSTs kills/deaths to IDUNA for every active,
// authenticated (has_player_id) client. Best-effort: IDUNA being briefly
// unreachable must never block match completion or the round timer, so
// every failure here is logged and swallowed rather than fatal — unlike
// ticket verification (S156-02), which fails closed because it gates
// identity, not telemetry.
static void report_match_results(void) {
    if (!iduna_agent_configured) return;

    char auth_body[512];
    snprintf(auth_body, sizeof(auth_body),
             "{\"agent_name\":\"%s\",\"agent_secret\":\"%s\"}",
             iduna_agent_name, iduna_agent_secret);

    char auth_resp[4096];
    int auth_status = 0;
    if (http_post_json(iduna_host, iduna_port, "/api/v1/auth/agent", NULL,
                        auth_body, auth_resp, sizeof(auth_resp), &auth_status) != 0 ||
        auth_status != 200) {
        printf("MATCH_REPORT: failed to authenticate as %s (status=%d) -- skipping result upload this match\n",
               iduna_agent_name, auth_status);
        return;
    }

    char token[2048];
    if (!http_extract_json_string_field(auth_resp, "access_token", token, sizeof(token))) {
        printf("MATCH_REPORT: auth response had no access_token field -- skipping result upload this match\n");
        return;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        PlayerState *p = &local_state.players[i];
        if (!slots[i].active || !p->has_player_id) continue;

        char uuid_str[37];
        uuid_bytes_to_string(p->player_id, uuid_str);

        char path[128];
        snprintf(path, sizeof(path), "/api/v1/players/%s/session", uuid_str);

        char body[128];
        snprintf(body, sizeof(body), "{\"kills\":%d,\"deaths\":%d}", p->kills, p->deaths);

        char resp[512];
        int status = 0;
        if (http_post_json(iduna_host, iduna_port, path, token, body, resp, sizeof(resp), &status) != 0) {
            printf("MATCH_REPORT: network error reporting client=%d player_id=%s\n", i, uuid_str);
        } else if (status != 200) {
            printf("MATCH_REPORT: IDUNA rejected result for client=%d player_id=%s status=%d\n", i, uuid_str, status);
        } else {
            printf("MATCH_REPORT: reported client=%d player_id=%s kills=%d deaths=%d\n", i, uuid_str, p->kills, p->deaths);
        }
    }
}

static int recorder_pick_target() {
    if (recorder.target_id >= 0 && recorder.target_id < MAX_CLIENTS) {
        if (local_state.players[recorder.target_id].active) {
            return recorder.target_id;
        }
    }
    int best_id = -1;
    int best_kills = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!local_state.players[i].active) continue;
        if (local_state.players[i].kills > best_kills) {
            best_kills = local_state.players[i].kills;
            best_id = i;
        }
    }
    return best_id;
}

static float recorder_compute_zoom(float dist) {
    if (dist > 120.0f) return 3.0f;
    if (dist > 70.0f) return 2.4f;
    if (dist > 35.0f) return 1.8f;
    return 1.2f;
}

static void recorder_init_file(const char *path) {
    if (!recorder.enabled) return;
    recorder.file = fopen(path, "w");
    if (!recorder.file) {
        printf("[REC] Failed to open recording file: %s\n", path);
        recorder.enabled = 0;
        return;
    }
    recorder.cam_x = RECORDER_NORTH_X;
    recorder.cam_y = RECORDER_NORTH_Y;
    recorder.cam_z = RECORDER_NORTH_Z;
    recorder.cam_yaw = 0.0f;
    recorder.cam_pitch = 0.0f;
    recorder.cam_zoom = 1.2f;
    fprintf(recorder.file, "; SHANKPIT Recorder v1 (Lisp-ASM)\n");
    fprintf(recorder.file, "(begin-recording :dt-ms 16 :north-start '(%.2f %.2f %.2f))\n",
            RECORDER_NORTH_X, RECORDER_NORTH_Y, RECORDER_NORTH_Z);
}

static void recorder_update_camera() {
    int target_id = recorder_pick_target();
    if (target_id < 0) return;
    PlayerState *target = &local_state.players[target_id];

    float desired_x = RECORDER_NORTH_X + target->x * 0.15f;
    float desired_y = RECORDER_NORTH_Y + target->y * 0.1f;
    float desired_z = RECORDER_NORTH_Z + target->z * 0.15f;

    recorder.cam_x += (desired_x - recorder.cam_x) * RECORDER_SMOOTH_POS;
    recorder.cam_y += (desired_y - recorder.cam_y) * RECORDER_SMOOTH_POS;
    recorder.cam_z += (desired_z - recorder.cam_z) * RECORDER_SMOOTH_POS;

    float dx = target->x - recorder.cam_x;
    float dy = (target->y + 2.0f) - recorder.cam_y;
    float dz = target->z - recorder.cam_z;
    float dist = sqrtf(dx * dx + dz * dz);
    float target_yaw = atan2f(dx, dz) * (180.0f / 3.14159f);
    float target_pitch = atan2f(dy, dist) * (180.0f / 3.14159f);

    recorder.cam_yaw += (target_yaw - recorder.cam_yaw) * RECORDER_SMOOTH_ANGLE;
    recorder.cam_pitch += (target_pitch - recorder.cam_pitch) * RECORDER_SMOOTH_ANGLE;

    float zoom = recorder_compute_zoom(dist);
    if (target->current_weapon == WPN_SNIPER) {
        zoom += 0.4f;
    }
    float shake_pos = RECORDER_SHAKE_POS / zoom;
    float shake_ang = RECORDER_SHAKE_ANGLE / zoom;

    recorder.cam_x += phys_rand_f() * shake_pos;
    recorder.cam_y += phys_rand_f() * shake_pos;
    recorder.cam_z += phys_rand_f() * shake_pos;
    recorder.cam_yaw += phys_rand_f() * shake_ang;
    recorder.cam_pitch += phys_rand_f() * shake_ang;
    recorder.cam_zoom = zoom;
}

static void recorder_write_frame(unsigned int tick, unsigned int now_ms) {
    if (!recorder.enabled || !recorder.file) return;
    recorder_update_camera();

    fprintf(recorder.file, "(frame :tick %u :time-ms %u\n", tick, now_ms);
    fprintf(recorder.file, "  (camera :x %.3f :y %.3f :z %.3f :yaw %.2f :pitch %.2f :zoom %.2f :mode \"handicam\")\n",
            recorder.cam_x, recorder.cam_y, recorder.cam_z,
            recorder.cam_yaw, recorder.cam_pitch, recorder.cam_zoom);


    for (int i = 0; i < MAX_CLIENTS; i++) {
        PlayerState *p = &local_state.players[i];
        if (!p->active) continue;
        fprintf(recorder.file,
                "  (actor :id %d :x %.3f :y %.3f :z %.3f :vx %.3f :vy %.3f :vz %.3f :yaw %.2f :pitch %.2f :weapon %d :state %d)\n",
                i, p->x, p->y, p->z, p->vx, p->vy, p->vz, p->yaw, p->pitch, p->current_weapon, p->state);
    }
    fprintf(recorder.file, ")\n");
    if (tick % 60 == 0) {
        fflush(recorder.file);
    }
}

int parse_server_mode(int argc, char **argv) {
    int mode = MODE_DEATHMATCH;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tdm") == 0) {
            mode = MODE_TDM;
        } else if (strcmp(argv[i], "--deathmatch") == 0) {
            mode = MODE_DEATHMATCH;
        }
    }
    return mode;
}

// parse_match_minutes reads --match-minutes N (default DEFAULT_MATCH_MINUTES).
// A value <= 0 disables the round boundary entirely (match never completes) —
// useful for the emily-bot E2E harness to isolate other behavior from round
// resets, or for a "practice server" that never scores.
int parse_match_minutes(int argc, char **argv) {
    int minutes = DEFAULT_MATCH_MINUTES;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--match-minutes") == 0 && i + 1 < argc) {
            minutes = atoi(argv[i + 1]);
            i++;
        }
    }
    return minutes;
}

// complete_match logs final standings and starts a new round: kills/deaths
// reset to zero for every active player, everyone respawns fresh (matching
// the "closed, non-redeemable economy" doctrine — nothing carries between
// rounds, see shankpit-460/docs2/NORTHSTAR.md §1), and the round timer
// restarts. This is the first real match/round boundary this server has had
// (EMILY/BACKLOG.md S156-01) — previously local_init_match ran once at
// startup and nothing ever closed a round.
void complete_match(int match_minutes) {
    local_state.match_number++;
    printf("MATCH_COMPLETE number=%d\n", local_state.match_number);
    report_match_results(); // S156-04 — must run before kills/deaths reset below
    for (int i = 0; i < MAX_CLIENTS; i++) {
        PlayerState *p = &local_state.players[i];
        if (!p->active) continue;
        printf("  standing client=%d kills=%d deaths=%d\n", i, p->kills, p->deaths);
        p->kills = 0;
        p->deaths = 0;
        if (i > 0) {
            phys_respawn(p, get_server_time());
        }
    }
    local_state.match_ticks_remaining = match_minutes > 0 ? match_minutes * TICKS_PER_MINUTE : 0;
}

void server_net_init() {
    setbuf(stdout, NULL);
    #ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
    #endif
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    #ifdef _WIN32
    u_long mode = 1; ioctlsocket(sock, FIONBIO, &mode);
    #else
    int flags = fcntl(sock, F_GETFL, 0); fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    #endif
    // $SHANKPIT_PORT overrides the default so a second instance (map
    // playtests, bot-eval runs) can coexist with the deployed server.
    int port = 6969;
    const char *port_env = getenv("SHANKPIT_PORT");
    if (port_env && atoi(port_env) > 0) port = atoi(port_env);
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons((unsigned short)port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        printf("FAILED TO BIND PORT %d\n", port);
        exit(1);
    } else {
        printf("SERVER LISTENING ON PORT %d\nWaiting...\n", port);
    }
}

void process_user_cmd(int client_id, UserCmd *cmd) {
    if (cmd->sequence <= client_last_seq[client_id]) return;
    PlayerState *p = &local_state.players[client_id];
    shankpit_apply_usercmd_inputs(p, cmd);
    client_last_seq[client_id] = cmd->sequence;
}

void server_handle_packet(struct sockaddr_in *sender, char *buffer, int size) {
    if (size < (int)sizeof(NetHeader)) return;
    NetHeader *head = (NetHeader*)buffer;

    if (head->type == PACKET_CONNECT) {
        // Verify the connect ticket BEFORE allocating any slot — an
        // invalid/missing/expired ticket (or an unconfigured secret, which
        // fails closed) is silently dropped: no slot consumed, no Welcome
        // sent. See EMILY/BACKLOG.md S156-02.
        unsigned char player_id[16];
        if (!verify_connect_ticket(buffer, size, player_id)) {
            return;
        }
        // One seat per identity (VS2 hard constraint).
        int existing = find_slot_by_player_id(player_id);
        if (existing != -1) {
            printf("CONNECT rejected: player_id already connected in slot=%d\n", existing);
            return;
        }

        int client_id = ensure_slot_for_sender(sender);
        if (client_id == -1) return;

        PlayerState *p = &local_state.players[client_id];
        memcpy(p->player_id, player_id, 16);
        p->has_player_id = 1;

        client_last_seq[client_id] = 0;
        p->in_fwd = 0.0f;
        p->in_strafe = 0.0f;
        p->in_jump = 0;
        p->in_shoot = 0;
        p->in_reload = 0;
        p->in_use = 0;
        p->in_ability = 0;
        p->use_was_down = 0;
        p->portal_cooldown_until_ms = 0;
        p->vehicle_cooldown = 0;
        send_welcome(sender, client_id);
        return;
    }

    int client_id = -1;
    if (head->type == PACKET_USERCMD || head->type == PACKET_DISCONNECT) {
        // Look up an existing slot only — never auto-create one here. A slot
        // is only allocated by the verified PACKET_CONNECT path above; if we
        // called ensure_slot_for_sender (which allocates+welcomes unknown
        // senders) here too, any client could skip CONNECT entirely, send a
        // bare USERCMD, and get silently welcomed with no ticket at all —
        // a total bypass of S156-02 ticket verification. Caught live via
        // emily-bot -bad-ticket/-no-ticket both incorrectly reporting
        // "welcomed" during S156-02 testing.
        client_id = find_slot_by_addr(sender);
    }
    if (client_id == -1) return;

    if (head->type == PACKET_DISCONNECT) {
        free_slot(client_id);
        return;
    }

    // --- USER COMMANDS ---
    if (client_id != -1 && head->type == PACKET_USERCMD) {
        int cursor = (int)sizeof(NetHeader);
        if (size < cursor + 1) return;

        unsigned char count = *(unsigned char*)(buffer + cursor); cursor += 1;

        if (size >= cursor + (int)(count * sizeof(UserCmd))) {
            UserCmd *cmds = (UserCmd*)(buffer + cursor);

            // process oldest->newest to preserve chronological intent
            for (int i = (int)count - 1; i >= 0; i--) {
                process_user_cmd(client_id, &cmds[i]);
            }

            slots[client_id].last_heard = now_seconds();
            local_state.client_meta[client_id].last_heard_ms = get_server_time();
            slots[client_id].cmd_seen = 1;
            local_state.players[client_id].active = slots[client_id].welcomed && slots[client_id].cmd_seen;
            local_state.client_meta[client_id].active = local_state.players[client_id].active;
        }
    }
}

void server_broadcast() {
    char buffer[4096];
    int cursor = 0;
    NetHeader head;
    head.type = PACKET_SNAPSHOT;
    head.client_id = 0;
    head.sequence = local_state.server_tick;
    head.timestamp = get_server_time();
    head.scene_id = 0;

    unsigned char count = 0;
    for(int i=1; i<MAX_CLIENTS; i++) if (slots[i].active && slots[i].welcomed && slots[i].cmd_seen && local_state.players[i].active) count++;
    head.entity_count = count;

    memcpy(buffer + cursor, &head, sizeof(NetHeader)); cursor += (int)sizeof(NetHeader);
    memcpy(buffer + cursor, &count, 1); cursor += 1;

    for(int i=1; i<MAX_CLIENTS; i++) {
        PlayerState *p = &local_state.players[i];
        if (slots[i].active && slots[i].welcomed && slots[i].cmd_seen && p->active) {
            NetPlayer np;
            np.id = (unsigned char)i;
            np.scene_id = (unsigned char)p->scene_id;
            np.last_seq = client_last_seq[i];
            np.x = p->x; np.y = p->y; np.z = p->z;
            np.yaw = norm_yaw_deg(p->yaw); np.pitch = clamp_pitch_deg(p->pitch);
            np.current_weapon = (unsigned char)p->current_weapon;
            np.state = (unsigned char)p->state;
            np.health = (unsigned char)p->health;
            np.shield = (unsigned char)p->shield;
            np.is_shooting = (unsigned char)p->is_shooting;
            np.crouching = (unsigned char)p->crouching;
            np.reward_feedback = p->accumulated_reward;
            np.ammo = (unsigned char)p->ammo[p->current_weapon];
            np.in_vehicle = (unsigned char)p->in_vehicle;
            np.hit_feedback = (unsigned char)p->hit_feedback;
            np.storm_charges = (unsigned char)p->storm_charges;

            p->accumulated_reward = 0;
            memcpy(buffer + cursor, &np, sizeof(NetPlayer)); cursor += (int)sizeof(NetPlayer);
        }
    }

    for(int i=1; i<MAX_CLIENTS; i++) {
        if (slots[i].active) {
            sendto(sock, buffer, cursor, 0,
                   (struct sockaddr*)&slots[i].addr,
                   sizeof(struct sockaddr_in));
        }
    }
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--record") == 0) {
            recorder.enabled = 1;
        } else if (strcmp(argv[i], "--record-target") == 0 && i + 1 < argc) {
            recorder.target_id = atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--record-file") == 0 && i + 1 < argc) {
            recorder.enabled = 1;
            recorder_init_file(argv[i + 1]);
            i++;
        }
    }

    if (recorder.enabled && !recorder.file) {
        recorder_init_file("shankpit_recording.lispasm");
    }

    server_net_init();
    load_ticket_secret();
    load_iduna_agent_config();
    map_load_default(); // $SHANKPIT_MAP or maps/v0_shankpit.map; falls back to built-in stadium
    int mode = parse_server_mode(argc, argv);
    int match_minutes = parse_match_minutes(argc, argv);
    local_init_match(1, mode);
    local_state.players[0].active = 0;
    local_state.players[0].health = 0;
    local_state.players[0].state = STATE_DEAD;
    local_state.match_ticks_remaining = match_minutes > 0 ? match_minutes * TICKS_PER_MINUTE : 0;
    local_state.match_number = 1;
    printf("SERVER MODE: %s\n", mode == MODE_TDM ? "TEAM DEATHMATCH" : "DEATHMATCH");
    if (match_minutes > 0) {
        printf("MATCH_LENGTH minutes=%d\n", match_minutes);
    } else {
        printf("MATCH_LENGTH disabled (--match-minutes <= 0) — round boundary off\n");
    }

    int running = 1;
    unsigned int tick = 0;

    while(running) {
        char buffer[1024];
        struct sockaddr_in sender;
        socklen_t slen = sizeof(sender);

        int len = recvfrom(sock, buffer, 1024, 0, (struct sockaddr*)&sender, &slen);
        while (len > 0) {
            server_handle_packet(&sender, buffer, len);
            len = recvfrom(sock, buffer, 1024, 0, (struct sockaddr*)&sender, &slen);
        }

        unsigned int now = get_server_time();
        // TIMEOUT_SWEEP
        for (int i = 1; i < MAX_CLIENTS; i++) {
            if (slots[i].active && now_seconds() - slots[i].last_heard > 5.0) {
                free_slot(i);
            }
        }

        int active_count = 0;

        for(int i=0; i<MAX_CLIENTS; i++) {
            PlayerState *p = &local_state.players[i];

            // Respawn-on-death is handled by update_entity's
            // respawn_delay_ticks countdown (packages/simulation/local_game.h,
            // called below for STATE_DEAD players) — not here. This used to
            // duplicate that with a timestamp check against p->respawn_time,
            // but respawn_time is always 0 (nothing sets it otherwise), so
            // `now > p->respawn_time` was always true — an unconditional,
            // same-tick respawn that raced ahead of any real delay. See
            // EMILY/BACKLOG.md SECTION 155 S155-03.

            if (p->active && p->state != STATE_DEAD) {
                phys_set_scene(p->scene_id);
                int use_pressed = p->in_use && !p->use_was_down;
                int portal_id = -1;
                if (use_pressed && now >= p->portal_cooldown_until_ms &&
                    scene_portal_active(p->scene_id) && scene_portal_triggered(p, &portal_id)) {
                    int dest_scene = -1;
                    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
                    if (portal_resolve_destination(p->scene_id, portal_id, p->id,
                                                   &dest_scene, &sx, &sy, &sz)) {
                        int from_scene = p->scene_id;
                        p->scene_id = dest_scene;
                        phys_set_scene(p->scene_id);
                        p->x = sx; p->y = sy; p->z = sz;
                        p->vx = 0.0f; p->vy = 0.0f; p->vz = 0.0f;
                        p->in_vehicle = 0;
                        p->portal_cooldown_until_ms = now + 1000;
                        p->in_use = 0;
                        printf("PORTAL_TRAVEL client=%d from=%d to=%d\n", i, from_scene, dest_scene);
                    }
                } else if (use_pressed && p->vehicle_cooldown == 0) {
                    int in_garage = p->scene_id == SCENE_GARAGE_OSAKA;
                    if (in_garage && scene_near_vehicle_pad(p->scene_id, p->x, p->z, 6.0f, NULL)) {
                        p->in_vehicle = !p->in_vehicle;
                        p->vehicle_cooldown = 30;
                        printf("Client %d Toggle Vehicle: %d\n", i, p->in_vehicle);
                    } else if (!in_garage) {
                        p->in_vehicle = !p->in_vehicle;
                        p->vehicle_cooldown = 30;
                        printf("Client %d Toggle Vehicle: %d\n", i, p->in_vehicle);
                    }
                }
                p->use_was_down = p->in_use;
                if (p->vehicle_cooldown > 0) p->vehicle_cooldown--;

                shankpit_simulate_movement_tick(p, now);
            } else {
                update_entity(p, SHANKPIT_NET_FIXED_DT, NULL, now);
            }
        }

        update_projectiles(now);
        recorder_write_frame(tick, now);
        if ((tick % SERVER_SNAPSHOT_INTERVAL_TICKS) == 0) {
            server_broadcast();
        }

        int connected = 0;
        for (int i = 1; i < MAX_CLIENTS; i++) {
            if (slots[i].active && slots[i].welcomed && slots[i].cmd_seen && local_state.players[i].active) connected++;
        }
        active_count = connected;
        
        if (tick % 60 == 0) {
            printf("[STATUS] Tick: %u | Clients: %d\n", tick, active_count);
            for (int i = 1; i < MAX_CLIENTS; i++) {
                if (!slots[i].active && !slots[i].welcomed && !slots[i].cmd_seen) continue;
                printf("  slot=%d active=%d welcomed=%d cmd_seen=%d player_active=%d last_heard_ms=%u\n",
                    i,
                    slots[i].active,
                    slots[i].welcomed,
                    slots[i].cmd_seen,
                    local_state.players[i].active,
                    local_state.client_meta[i].last_heard_ms);
            }
        }

        if (local_state.match_ticks_remaining > 0) {
            local_state.match_ticks_remaining--;
            if (local_state.match_ticks_remaining == 0) {
                complete_match(match_minutes);
            }
        }

        local_state.server_tick++;

        #ifdef _WIN32
        Sleep(16);
        #else
        usleep(16000);
        #endif

        tick++;
    }

    return 0;
}
