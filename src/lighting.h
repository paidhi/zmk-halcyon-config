/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Lighting state the central owns and sends to the peripheral. */
struct elora_lighting_state {
    uint16_t layers;    /* active layer IDs, default layer included */
    uint8_t top_layer;  /* highest active layer ID */
    bool enabled;       /* user setting: lights on */
    bool awake;         /* lighting idle timer not expired */
    bool per_key;       /* stage 2 per-key colors; always false for now */
    uint8_t brightness; /* percent */
};

/* Pack a state into the sync behavior's two parameters. */
void elora_lighting_encode(const struct elora_lighting_state *state, uint8_t sequence,
                           uint32_t *param1, uint32_t *param2);

/* Unpack the sync behavior's parameters; returns -EINVAL for an unknown format. */
int elora_lighting_decode(uint32_t param1, uint32_t param2, struct elora_lighting_state *state);

/* Render a state on this half. */
void elora_lighting_show(const struct elora_lighting_state *state);
