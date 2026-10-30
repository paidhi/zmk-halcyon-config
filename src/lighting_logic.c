/*
 * SPDX-License-Identifier: MIT
 *
 * Lighting logic without Zephyr dependencies, shared by both halves and the
 * host tests: the snapshot codec, brightness scaling, and key color
 * resolution.
 */

#include <errno.h>

#include "lighting.h"

/* Snapshot layout, see plans/elora-layer-rgb.md in the parent workspace. */
#define SNAPSHOT_VERSION 1u
#define P1_LAYERS_MASK 0x0000ffffu
#define P1_TOP_SHIFT 16
#define P1_TOP_MASK 0x000f0000u
#define P1_SEQ_SHIFT 24
#define P1_SEQ_MASK 0xff000000u
#define P2_ENABLED 0x00000001u
#define P2_AWAKE 0x00000002u
#define P2_PER_KEY 0x00000004u
#define P2_BRT_SHIFT 8
#define P2_BRT_MASK 0x0000ff00u
#define P2_VERSION_SHIFT 28
#define P2_VERSION_MASK 0xf0000000u

void elora_lighting_encode(const struct elora_lighting_state *state, uint8_t sequence,
                           uint32_t *param1, uint32_t *param2) {
    *param1 = state->layers | ((uint32_t)(state->top_layer & 0xf) << P1_TOP_SHIFT) |
              ((uint32_t)sequence << P1_SEQ_SHIFT);
    *param2 = (state->enabled ? P2_ENABLED : 0) | (state->awake ? P2_AWAKE : 0) |
              (state->per_key ? P2_PER_KEY : 0) | ((uint32_t)state->brightness << P2_BRT_SHIFT) |
              (SNAPSHOT_VERSION << P2_VERSION_SHIFT);
}

int elora_lighting_decode(uint32_t param1, uint32_t param2, struct elora_lighting_state *state) {
    const uint32_t p1_known = P1_LAYERS_MASK | P1_TOP_MASK | P1_SEQ_MASK;
    const uint32_t p2_known = P2_ENABLED | P2_AWAKE | P2_PER_KEY | P2_BRT_MASK | P2_VERSION_MASK;

    if ((param2 >> P2_VERSION_SHIFT) != SNAPSHOT_VERSION || (param1 & ~p1_known) ||
        (param2 & ~p2_known)) {
        return -EINVAL;
    }

    *state = (struct elora_lighting_state){
        .layers = param1 & P1_LAYERS_MASK,
        .top_layer = (param1 & P1_TOP_MASK) >> P1_TOP_SHIFT,
        .enabled = param2 & P2_ENABLED,
        .awake = param2 & P2_AWAKE,
        .per_key = param2 & P2_PER_KEY,
        .brightness = (param2 & P2_BRT_MASK) >> P2_BRT_SHIFT,
    };
    return 0;
}

uint8_t elora_lighting_sequence(uint32_t param1) { return param1 >> P1_SEQ_SHIFT; }

uint32_t elora_lighting_scale(uint32_t color, uint8_t percent) {
    uint32_t r = ((color >> 16) & 0xff) * percent / 100;
    uint32_t g = ((color >> 8) & 0xff) * percent / 100;
    uint32_t b = (color & 0xff) * percent / 100;

    return (r << 16) | (g << 8) | b;
}

uint32_t elora_lighting_key_color(const struct elora_lighting_keymap *map, uint16_t layers,
                                  uint8_t position) {
    if (position >= map->positions) {
        return 0;
    }

    for (int layer = map->layers - 1; layer >= 0; layer--) {
        if (!(layers & (1u << layer))) {
            continue;
        }

        size_t index = (size_t)layer * map->positions + position;
        switch (map->kinds[index]) {
        case ELORA_KEY_TRANS:
            continue;
        case ELORA_KEY_NONE:
            return 0;
        default:
            return map->colors[index];
        }
    }

    return 0;
}
