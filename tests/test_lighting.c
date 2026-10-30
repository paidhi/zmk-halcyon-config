/*
 * SPDX-License-Identifier: MIT
 *
 * Host tests for the lighting logic: snapshot codec, brightness scaling, and
 * key color resolution. Build and run them in the ZMK build container, see
 * BUILDING.md in the parent workspace.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "lighting.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);               \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void test_snapshot_round_trip(void) {
    const struct elora_lighting_state in = {
        .layers = 0x0119,
        .top_layer = 8,
        .enabled = true,
        .awake = true,
        .per_key = true,
        .brightness = 25,
    };
    struct elora_lighting_state out;
    uint32_t p1, p2;

    elora_lighting_encode(&in, 0xa5, &p1, &p2);
    CHECK(p1 == 0xa5080119u);
    CHECK(p2 == 0x10001907u);
    CHECK(elora_lighting_sequence(p1) == 0xa5);
    CHECK(elora_lighting_decode(p1, p2, &out) == 0);
    CHECK(out.layers == in.layers && out.top_layer == in.top_layer);
    CHECK(out.enabled && out.awake && out.per_key && out.brightness == 25);

    const struct elora_lighting_state off = {.layers = 1, .brightness = 5};
    elora_lighting_encode(&off, 0, &p1, &p2);
    CHECK(elora_lighting_decode(p1, p2, &out) == 0);
    CHECK(!out.enabled && !out.awake && !out.per_key && out.brightness == 5);
}

static void test_snapshot_rejects_unknown(void) {
    const struct elora_lighting_state in = {.layers = 1, .enabled = true, .brightness = 10};
    struct elora_lighting_state out;
    uint32_t p1, p2;

    elora_lighting_encode(&in, 1, &p1, &p2);
    CHECK(elora_lighting_decode(p1 | 0x00100000u, p2, &out) == -EINVAL); /* p1 reserved */
    CHECK(elora_lighting_decode(p1, p2 | 0x00000008u, &out) == -EINVAL); /* p2 reserved */
    CHECK(elora_lighting_decode(p1, p2 | 0x00010000u, &out) == -EINVAL); /* p2 reserved */
    CHECK(elora_lighting_decode(p1, (p2 & 0x0fffffffu) | 0x20000000u, &out) == -EINVAL);
    CHECK(elora_lighting_decode(p1, p2 & 0x0fffffffu, &out) == -EINVAL);
}

static void test_scale(void) {
    CHECK(elora_lighting_scale(0xffffff, 100) == 0xffffff);
    CHECK(elora_lighting_scale(0xff8000, 10) == 0x190c00);
    CHECK(elora_lighting_scale(0xc0c0c0, 5) == 0x090909);
    CHECK(elora_lighting_scale(0x123456, 0) == 0);
}

/* Four layers of four positions: base, a lower layer, and two overlays. */
#define B ELORA_KEY_BOUND
#define T ELORA_KEY_TRANS
#define N ELORA_KEY_NONE

static const uint8_t kinds[4][4] = {
    {B, B, B, B}, /* 0: base, no roles */
    {T, B, T, B}, /* 1: lower layer with a role at 1 */
    {B, T, N, T}, /* 2: role at 0, no role at 3 below */
    {T, T, B, N}, /* 3: role at 2 */
};
static const uint32_t colors[4][4] = {
    {0, 0, 0, 0},
    {0, 0x000011, 0, 0x000033},
    {0x110000, 0, 0, 0},
    {0, 0, 0x001100, 0},
};
static const struct elora_lighting_keymap map = {
    .layers = 4,
    .positions = 4,
    .kinds = &kinds[0][0],
    .colors = &colors[0][0],
};

static void test_resolution(void) {
    /* Base only: no roles anywhere. */
    for (uint8_t pos = 0; pos < 4; pos++) {
        CHECK(elora_lighting_key_color(&map, 0x1, pos) == 0);
    }

    /* Layer 2 over base: its own role, &trans to an unlit base, &none. */
    CHECK(elora_lighting_key_color(&map, 0x5, 0) == 0x110000);
    CHECK(elora_lighting_key_color(&map, 0x5, 1) == 0);
    CHECK(elora_lighting_key_color(&map, 0x5, 2) == 0);

    /* Layer 2 over layer 1: &trans shows layer 1's role. */
    CHECK(elora_lighting_key_color(&map, 0x7, 1) == 0x000011);
    CHECK(elora_lighting_key_color(&map, 0x7, 3) == 0x000033);

    /* Layer 3 on top: its role, &none stops before layer 1's role, and two
     * &trans in a row reach layer 1 past the inactive layer 2. */
    CHECK(elora_lighting_key_color(&map, 0xf, 2) == 0x001100);
    CHECK(elora_lighting_key_color(&map, 0xf, 3) == 0);
    CHECK(elora_lighting_key_color(&map, 0xb, 1) == 0x000011);
    CHECK(elora_lighting_key_color(&map, 0xf, 0) == 0x110000);

    /* Inactive layers do not count, even with a higher ID. */
    CHECK(elora_lighting_key_color(&map, 0x3, 0) == 0);

    /* Positions and layers outside the map are dark. */
    CHECK(elora_lighting_key_color(&map, 0xf, 4) == 0);
    CHECK(elora_lighting_key_color(&map, 0x10, 0) == 0);
}

int main(void) {
    test_snapshot_round_trip();
    test_snapshot_rejects_unknown();
    test_scale();
    test_resolution();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("lighting tests passed\n");
    return EXIT_SUCCESS;
}
