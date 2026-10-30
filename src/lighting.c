/*
 * SPDX-License-Identifier: MIT
 *
 * Layer lighting renderer, shared by both halves.
 *
 * Writes the full chain on every change: the underglow pixels get the color
 * of the highest active layer (unless its key colors replace it) and, with
 * per-key colors on, each key pixel
 * gets the color of its key's role on the active layers, all scaled by
 * brightness. Every other pixel is black. The LED supply is switched off
 * whenever nothing is lit. The peripheral receives its state through the
 * lit_sync behavior.
 */

#define DT_DRV_COMPAT elora_behavior_lighting_sync

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include <drivers/behavior.h>
#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/events/split_peripheral_status_changed.h>
#endif

#include "lighting.h"

LOG_MODULE_REGISTER(elora_lighting, CONFIG_ZMK_LOG_LEVEL);

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)
#define SUPPLY_NODE DT_INST(0, zmk_ext_power_generic)
#define SUPPLY_SETTLE_MS DT_PROP_OR(SUPPLY_NODE, init_delay_ms, 50)
#define DATA_NODE DT_INST(0, elora_lighting)

BUILD_ASSERT(DT_NODE_HAS_PROP(DATA_NODE, underglow_pixels),
             "elora_lighting needs underglow-pixels in this half's config overlay");
BUILD_ASSERT(DT_NODE_HAS_PROP(DATA_NODE, key_pixels) &&
                 DT_PROP_LEN(DATA_NODE, key_pixels) % 2 == 0,
             "elora_lighting needs key-pixels pairs in this half's config overlay");
BUILD_ASSERT(ZMK_KEYMAP_LAYERS_LEN <= 16, "the snapshot carries at most 16 layers");
BUILD_ASSERT(ZMK_KEYMAP_LEN <= UINT8_MAX, "key positions must fit in a byte");

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static const struct gpio_dt_spec supply = GPIO_DT_SPEC_GET_BY_IDX(SUPPLY_NODE, control_gpios, 0);
static const uint32_t layer_colors[] = DT_PROP(DATA_NODE, layer_colors);
static const uint8_t underglow_pixels[] = DT_PROP(DATA_NODE, underglow_pixels);
/* Pairs of pixel index and key position. */
static const uint8_t key_pixels[] = DT_PROP(DATA_NODE, key_pixels);

/* Layers whose underglow stays dark while per-key colors are on. */
#define LAYER_BIT(node, prop, idx) | BIT(DT_PROP_BY_IDX(node, prop, idx))
static const uint16_t keys_replace_underglow =
    0 COND_CODE_1(DT_NODE_HAS_PROP(DATA_NODE, keys_replace_underglow),
                  (DT_FOREACH_PROP_ELEM(DATA_NODE, keys_replace_underglow, LAYER_BIT)), ());

/* Binding kinds from the compiled keymap, in layer ID order. */
#define BINDING_IS(layer, idx, label)                                                              \
    DT_SAME_NODE(DT_PHANDLE_BY_IDX(layer, bindings, idx), DT_NODELABEL(label))
#define BINDING_KIND(idx, layer)                                                                   \
    (BINDING_IS(layer, idx, trans)  ? ELORA_KEY_TRANS                                              \
     : BINDING_IS(layer, idx, none) ? ELORA_KEY_NONE                                               \
                                    : ELORA_KEY_BOUND)
#define LAYER_KINDS(layer) {LISTIFY(DT_PROP_LEN(layer, bindings), BINDING_KIND, (, ), layer)}

static const uint8_t key_kinds[ZMK_KEYMAP_LAYERS_LEN][ZMK_KEYMAP_LEN] = {
    ZMK_KEYMAP_LAYERS_FOREACH_SEP(LAYER_KINDS, (, ))};

/* Key colors from the role groups under the elora_lighting node. */
#define ROLE_KEY(group, prop, idx)                                                                 \
    [DT_PROP(group, layer)][DT_PROP_BY_IDX(group, prop, idx)] = DT_PROP(group, color),
#define ROLE_GROUP(group) DT_FOREACH_PROP_ELEM(group, positions, ROLE_KEY)

static const uint32_t key_colors[ZMK_KEYMAP_LAYERS_LEN][ZMK_KEYMAP_LEN] = {
    DT_FOREACH_CHILD(DATA_NODE, ROLE_GROUP)};

static const struct elora_lighting_keymap role_map = {
    .layers = ZMK_KEYMAP_LAYERS_LEN,
    .positions = ZMK_KEYMAP_LEN,
    .kinds = &key_kinds[0][0],
    .colors = &key_colors[0][0],
};

/* State requested by the central (or decoded from a snapshot), guarded by
 * lock because events, the split link, and work items all update it. */
static struct k_spinlock lock;
static struct elora_lighting_state shown;
static bool have_state;
static bool suspended;

/* Used only by the render work item. */
static struct led_rgb pixels[NUM_PIXELS];
static bool supply_on;
static int64_t supply_ready_at;

static bool set_pixel(uint8_t index, uint32_t color) {
    if (index >= NUM_PIXELS) {
        return false;
    }

    pixels[index] = (struct led_rgb){
        .r = (color >> 16) & 0xff,
        .g = (color >> 8) & 0xff,
        .b = color & 0xff,
    };
    return color != 0;
}

/* Compose the frame for the requested state; returns whether any pixel is
 * lit. */
static bool compose(void) {
    struct elora_lighting_state state;
    bool lit;

    K_SPINLOCK(&lock) {
        state = shown;
        lit = have_state && !suspended;
    }

    memset(pixels, 0, sizeof(pixels));
    if (!lit || !state.enabled || !state.awake) {
        return false;
    }

    uint8_t percent = MIN(state.brightness, CONFIG_ELORA_LIGHTING_BRT_MAX);
    bool any = false;

    bool replaced = state.per_key && (keys_replace_underglow & BIT(state.top_layer));

    if (state.top_layer < ARRAY_SIZE(layer_colors) && !replaced) {
        uint32_t color = elora_lighting_scale(layer_colors[state.top_layer], percent);
        for (size_t i = 0; i < ARRAY_SIZE(underglow_pixels); i++) {
            any |= set_pixel(underglow_pixels[i], color);
        }
    }

    if (state.per_key) {
        for (size_t i = 0; i < ARRAY_SIZE(key_pixels); i += 2) {
            uint32_t color = elora_lighting_key_color(&role_map, state.layers, key_pixels[i + 1]);

            any |= set_pixel(key_pixels[i], elora_lighting_scale(color, percent));
        }
    }

    return any;
}

static void write_frame(void) {
    int err = led_strip_update_rgb(strip, pixels, NUM_PIXELS);
    if (err) {
        LOG_ERR("LED strip update failed (%d)", err);
    }
}

static void render_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(render_work, render_work_cb);

static void render_work_cb(struct k_work *work) {
    if (!compose()) {
        if (supply_on) {
            write_frame();
            gpio_pin_set_dt(&supply, 0);
            supply_on = false;
        }
        return;
    }

    if (!supply_on) {
        gpio_pin_set_dt(&supply, 1);
        supply_on = true;
        supply_ready_at = k_uptime_get() + SUPPLY_SETTLE_MS;
    }

    int64_t wait = supply_ready_at - k_uptime_get();
    if (wait > 0) {
        k_work_schedule(&render_work, K_MSEC(wait));
        return;
    }

    write_frame();
}

/* Does not shorten a pending wait for the supply to settle. */
static void request_render(void) { k_work_schedule(&render_work, K_NO_WAIT); }

void elora_lighting_show(const struct elora_lighting_state *state) {
    K_SPINLOCK(&lock) {
        shown = *state;
        have_state = true;
    }
    request_render();
}

static void forget_state(void) {
    K_SPINLOCK(&lock) { have_state = false; }
    request_render();
}

static int lighting_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *activity = as_zmk_activity_state_changed(eh);

    if (activity != NULL) {
        K_SPINLOCK(&lock) { suspended = activity->state != ZMK_ACTIVITY_ACTIVE; }
        request_render();
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    const struct zmk_split_peripheral_status_changed *status =
        as_zmk_split_peripheral_status_changed(eh);

    /* Stay dark without a central until it sends a fresh snapshot. */
    if (status != NULL && !status->connected) {
        forget_state();
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(elora_lighting, lighting_listener);
ZMK_SUBSCRIPTION(elora_lighting, zmk_activity_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(elora_lighting, zmk_split_peripheral_status_changed);
#endif

static int lighting_init(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&supply) || gpio_pin_configure_dt(&supply, GPIO_OUTPUT_INACTIVE)) {
        LOG_ERR("LED supply pin not ready");
        return -EIO;
    }

    for (size_t i = 0; i < ARRAY_SIZE(underglow_pixels); i++) {
        if (underglow_pixels[i] >= NUM_PIXELS) {
            LOG_ERR("underglow pixel %d is outside the %d-pixel chain", underglow_pixels[i],
                    NUM_PIXELS);
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(key_pixels); i += 2) {
        if (key_pixels[i] >= NUM_PIXELS || key_pixels[i + 1] >= ZMK_KEYMAP_LEN) {
            LOG_ERR("key pixel %d at position %d is outside the chain or keymap", key_pixels[i],
                    key_pixels[i + 1]);
        }
    }

    /* A role on &trans or &none never shows; flag it as a keymap mistake. */
    for (size_t layer = 0; layer < ZMK_KEYMAP_LAYERS_LEN; layer++) {
        for (size_t position = 0; position < ZMK_KEYMAP_LEN; position++) {
            if (key_colors[layer][position] && key_kinds[layer][position] != ELORA_KEY_BOUND) {
                LOG_WRN("layer %d position %d has a role but no binding", (int)layer,
                        (int)position);
            }
        }
    }

    return 0;
}

SYS_INIT(lighting_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* lit_sync: the central invokes it on the peripheral with a packed snapshot. */
static int lit_sync_pressed(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    struct elora_lighting_state state;

    if (elora_lighting_decode(binding->param1, binding->param2, &state)) {
        LOG_WRN("unknown snapshot format 0x%08x 0x%08x", binding->param1, binding->param2);
        forget_state();
        return ZMK_BEHAVIOR_OPAQUE;
    }

    LOG_DBG("snapshot %u: layer %u, enabled %d, awake %d, per-key %d, brightness %u",
            elora_lighting_sequence(binding->param1), state.top_layer, state.enabled, state.awake,
            state.per_key, state.brightness);
    elora_lighting_show(&state);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int lit_sync_released(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api lit_sync_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = lit_sync_pressed,
    .binding_released = lit_sync_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &lit_sync_driver_api);
