/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lighting state the central owns and sends to the peripheral. */
struct elora_lighting_state {
    uint16_t layers;    /* active layer IDs, default layer included */
    uint8_t top_layer;  /* highest active layer ID */
    bool enabled;       /* user setting: lights on */
    bool awake;         /* lighting idle timer not expired */
    bool per_key;       /* user setting: key colors on */
    uint8_t brightness; /* percent */
};

/* How a keymap binding takes part in key color resolution. */
enum elora_key_kind {
    ELORA_KEY_BOUND, /* any behavior other than the two below */
    ELORA_KEY_TRANS, /* &trans: the next active layer decides */
    ELORA_KEY_NONE,  /* &none: dark */
};

/* Key colors per layer and position, as layers x positions arrays in layer
 * ID order. A color of 0 means the key has no role. */
struct elora_lighting_keymap {
    uint8_t layers;
    uint8_t positions;
    const uint8_t *kinds;   /* enum elora_key_kind */
    const uint32_t *colors; /* 0xRRGGBB at full scale */
};

/* Pack a state into the sync behavior's two parameters. */
void elora_lighting_encode(const struct elora_lighting_state *state, uint8_t sequence,
                           uint32_t *param1, uint32_t *param2);

/* Unpack the sync behavior's parameters; returns -EINVAL for an unknown format. */
int elora_lighting_decode(uint32_t param1, uint32_t param2, struct elora_lighting_state *state);

/* The sequence number of a packed snapshot. */
uint8_t elora_lighting_sequence(uint32_t param1);

/* Scale a 0xRRGGBB color by a brightness percentage. */
uint32_t elora_lighting_scale(uint32_t color, uint8_t percent);

/* Full-scale color of a key position for a set of active layers: the highest
 * active layer that is not &trans decides, and &none or a key without a role
 * is dark (0). */
uint32_t elora_lighting_key_color(const struct elora_lighting_keymap *map, uint16_t layers,
                                  uint8_t position);

/* Render a state on this half. */
void elora_lighting_show(const struct elora_lighting_state *state);
