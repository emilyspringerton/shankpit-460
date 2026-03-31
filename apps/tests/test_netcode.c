
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#ifndef _WIN32
#include <netinet/in.h>
#endif
#include "../../packages/common/protocol.h"

// --- MICRO TEST FRAMEWORK ---
int tests_run = 0;
int tests_passed = 0;

#define ASSERT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) != (b)) { \
        printf("❌ FAIL: %s (Expected %d, Got %d)\n", msg, (int)(a), (int)(b)); \
    } else { \
        printf("✅ PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("❌ FAIL: %s\n", msg); \
    } else { \
        printf("✅ PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

// --- TESTS ---

void test_packet_structure() {
    printf("--- Testing Packet Overhead ---\n");
    size_t header_size = sizeof(NetHeader);
    size_t cmd_size = sizeof(UserCmd);
    size_t total_packet = header_size + cmd_size;
    
    printf("Header: %zu bytes | UserCmd: %zu bytes | Total: %zu bytes\n", header_size, cmd_size, total_packet);
    
    // Safety check: Is this way smaller than a standard MTU (1400)?
    ASSERT_TRUE(total_packet < 1400, "Packet fits comfortably in UDP MTU");
    ASSERT_TRUE(total_packet < 64, "Packet is hyper-optimized (Target < 64 bytes)");
}

void test_handshake_enums() {
    printf("--- Testing Handshake Types ---\n");
    // Ensure distinct values
    ASSERT_EQ(PACKET_CONNECT, 0, "Connect is type 0");
    ASSERT_EQ(PACKET_USERCMD, 1, "UserCmd is type 1");
    ASSERT_EQ(PACKET_SNAPSHOT, 2, "Snapshot is type 2");
    ASSERT_EQ(PACKET_WELCOME, 3, "Welcome is type 3");
    ASSERT_EQ(PACKET_DISCONNECT, 4, "Disconnect is type 4");

    // Verify differentiation
    ASSERT_TRUE(PACKET_CONNECT != PACKET_USERCMD, "Connect != UserCmd");
    ASSERT_TRUE(PACKET_DISCONNECT != PACKET_USERCMD, "Disconnect != UserCmd");
}

void test_simulation_delta() {
    printf("--- Testing Delta Time Limits ---\n");
    UserCmd cmd;
    cmd.msec = 16; // 60 FPS
    ASSERT_EQ(cmd.msec, 16, "Stores 16ms correctly");
    
    cmd.msec = 1000; // 1 FPS (Lag spike)
    ASSERT_EQ(cmd.msec, 1000, "Stores 1000ms lag spike correctly");
    
    // Max unsigned short is 65535, so we are safe from overflow unless lag is > 65 seconds
    ASSERT_TRUE(sizeof(cmd.msec) == 2, "msec is 2 bytes (ushort)");
}

void test_button_bits() {
    printf("--- Testing Button Bitmasks (Regression) ---\n");
    unsigned char btns = 0;
    btns |= BTN_JUMP | BTN_ATTACK;
    ASSERT_EQ((btns & BTN_JUMP), BTN_JUMP, "Jump bit set");
    ASSERT_EQ((btns & BTN_ATTACK), BTN_ATTACK, "Attack bit set");
    ASSERT_EQ((btns & BTN_CROUCH), 0, "Crouch bit NOT set");
}

void test_rotation_wire_roundtrip() {
    printf("--- Testing Rotation Wire Roundtrip ---\n");
    char buffer[256];
    int cursor = 0;
    NetHeader head;
    memset(&head, 0, sizeof(head));
    head.type = PACKET_USERCMD;
    memcpy(buffer + cursor, &head, sizeof(NetHeader));
    cursor += (int)sizeof(NetHeader);

    UserCmd in;
    memset(&in, 0, sizeof(in));
    in.yaw = 90.0f;
    in.pitch = 10.0f;
    memcpy(buffer + cursor, &in, sizeof(UserCmd));

    UserCmd out;
    memcpy(&out, buffer + sizeof(NetHeader), sizeof(UserCmd));

    ASSERT_TRUE(fabsf(out.yaw - 90.0f) < 0.0001f, "Yaw roundtrips over wire");
    ASSERT_TRUE(fabsf(out.pitch - 10.0f) < 0.0001f, "Pitch roundtrips over wire");
}

void test_scene_fields() {
    printf("--- Testing Scene Fields ---\n");
    ASSERT_EQ(sizeof(((NetHeader*)0)->scene_id), 1, "NetHeader scene_id is 1 byte");
    ASSERT_EQ(sizeof(((NetPlayer*)0)->scene_id), 1, "NetPlayer scene_id is 1 byte");
    NetPlayer np;
    np.scene_id = SCENE_STADIUM;
    ASSERT_EQ(np.scene_id, SCENE_STADIUM, "NetPlayer stores scene_id");
}

void test_katana_contract() {
    printf("--- Testing Katana Contract ---\n");
    ASSERT_EQ(MAX_WEAPONS, 6, "MAX_WEAPONS includes katana");
    ASSERT_EQ(WPN_KATANA, 5, "Katana weapon enum appended");
    ASSERT_EQ(WPN_STATS[WPN_KATANA].ammo_max, 0, "Katana uses no ammo");
    ASSERT_TRUE(sizeof(((PlayerState*)0)->dash_hit_targets) / sizeof(int) == 8, "Dash hit registry stored in PlayerState");
}

int main() {
    printf("🛡️ SHANKPIT PROTOCOL VERIFICATION 🛡️\n");
    test_packet_structure();
    test_handshake_enums();
    test_simulation_delta();
    test_button_bits();
    test_scene_fields();
    test_katana_contract();
    test_rotation_wire_roundtrip();
    
    printf("\n--------------------------------------\n");
    printf("SUMMARY: %d/%d Tests Passed.\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
