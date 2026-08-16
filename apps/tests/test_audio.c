/* S169-09: unit tests for audio_spatial_gains -- the one part of the
 * spatial audio port that's meaningfully testable without a real audio
 * device (weapon/footstep synthesis and mixing run on the SDL audio
 * thread against a real device, which this headless environment doesn't
 * have -- verified live: audio_synth_create() logs "SDL_OpenAudioDevice
 * failed" and returns cleanly, gameplay unaffected, same as SHANKPIT's
 * own original behavior in this same environment).
 */
#include <math.h>
#include <stdio.h>

#include "../../packages/audio/audio.h"

int tests_run = 0;
int tests_passed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
    } else { \
        printf("PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

#define ASSERT_NEAR(a, b, eps, msg) ASSERT_TRUE(fabsf((a) - (b)) < (eps), msg)

static void test_source_at_listener_position(void) {
    printf("--- Source at listener position (zero distance) ---\n");
    float l, r;
    audio_spatial_gains(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &l, &r);
    ASSERT_NEAR(l, 1.0f, 0.001f, "left gain is full volume at zero distance");
    ASSERT_NEAR(r, 1.0f, 0.001f, "right gain is full volume at zero distance");
}

static void test_source_directly_ahead(void) {
    printf("--- Source directly ahead of listener ---\n");
    /* listener at origin, yaw=0 (facing +Z per audio.h's own convention),
       source straight ahead on the +Z axis: should pan dead center. */
    float l, r;
    audio_spatial_gains(0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 0.0f, 0.0f, &l, &r);
    ASSERT_NEAR(l, r, 0.001f, "source directly ahead pans centered (L==R)");
}

static void test_source_to_the_right(void) {
    printf("--- Source to the listener's right ---\n");
    /* listener at origin facing +Z (yaw=0), source on the +X axis is to
       the listener's right -- right gain should exceed left gain. */
    float l, r;
    audio_spatial_gains(10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &l, &r);
    ASSERT_TRUE(r > l, "source to the right has higher right-channel gain than left");
}

static void test_source_to_the_left(void) {
    printf("--- Source to the listener's left ---\n");
    float l, r;
    audio_spatial_gains(-10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &l, &r);
    ASSERT_TRUE(l > r, "source to the left has higher left-channel gain than right");
}

static void test_distance_rolloff(void) {
    printf("--- Distance rolloff: farther source is quieter ---\n");
    float l_near, r_near, l_far, r_far;
    audio_spatial_gains(0.0f, 0.0f, 5.0f, 0.0f, 0.0f, 0.0f, 0.0f, &l_near, &r_near);
    audio_spatial_gains(0.0f, 0.0f, 500.0f, 0.0f, 0.0f, 0.0f, 0.0f, &l_far, &r_far);
    ASSERT_TRUE(l_near > l_far, "a near source is louder than a far source (left channel)");
    ASSERT_TRUE(r_near > r_far, "a near source is louder than a far source (right channel)");
}

static void test_listener_yaw_rotates_perceived_direction(void) {
    printf("--- Listener yaw rotates which side a fixed source is on ---\n");
    /* Source fixed on +X. Listener facing +Z (yaw=0) hears it on the
       right (per test_source_to_the_right). Turning the listener 180
       degrees (yaw=pi) should flip which channel is louder. */
    float l0, r0, l180, r180;
    audio_spatial_gains(10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &l0, &r0);
    audio_spatial_gains(10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 3.14159265f, &l180, &r180);
    ASSERT_TRUE(r0 > l0, "facing +Z: fixed +X source is on the right");
    ASSERT_TRUE(l180 > r180, "facing -Z (180 turn): the same +X source is now on the left");
}

static void test_gains_never_negative_or_unbounded(void) {
    printf("--- Gains stay within a sane [0,1] range ---\n");
    float l, r;
    /* Very close source -- rolloff formula should still clamp to <= 1.0. */
    audio_spatial_gains(0.001f, 0.0f, 0.001f, 0.0f, 0.0f, 0.0f, 0.0f, &l, &r);
    ASSERT_TRUE(l >= 0.0f && l <= 1.0f, "left gain in [0,1] for a very close source");
    ASSERT_TRUE(r >= 0.0f && r <= 1.0f, "right gain in [0,1] for a very close source");
}

int main(void) {
    printf("SHANKPIT-460 SPATIAL AUDIO CHECK (S169-09)\n");
    test_source_at_listener_position();
    test_source_directly_ahead();
    test_source_to_the_right();
    test_source_to_the_left();
    test_distance_rolloff();
    test_listener_yaw_rotates_perceived_direction();
    test_gains_never_negative_or_unbounded();
    printf("\n--------------------------------------\n");
    printf("SUMMARY: %d/%d Tests Passed.\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
