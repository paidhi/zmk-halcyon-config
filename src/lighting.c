/*
 * SPDX-License-Identifier: MIT
 *
 * Layer lighting renderer, shared by both halves.
 *
 * Writes the full chain on every change: the underglow pixels get the color
 * of the highest active layer, scaled by brightness, and every other pixel is
 * black. The LED supply is switched off whenever nothing is lit. The
 * peripheral receives its state through the lit_sync behavior.
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

/* Snapshot layout, see plans/elora-layer-rgb.md in the parent workspace. */
#define SNAPSHOT_VERSION 1
#define P1_LAYERS_MASK 0x0000ffffu
#define P1_TOP_SHIFT 16
#define P1_TOP_MASK 0x000f0000u
#define P1_SEQ_MASK 0xff000000u
#define P1_SEQ_SHIFT 24
#define P2_ENABLED BIT(0)
#define P2_AWAKE BIT(1)
#define P2_PER_KEY BIT(2)
#define P2_BRT_SHIFT 8
#define P2_BRT_MASK 0x0000ff00u
#define P2_VERSION_SHIFT 28
#define P2_VERSION_MASK 0xf0000000u

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static const struct gpio_dt_spec supply = GPIO_DT_SPEC_GET_BY_IDX(SUPPLY_NODE, control_gpios, 0);
static const uint32_t layer_colors[] = DT_PROP(DATA_NODE, layer_colors);
static const uint8_t underglow_pixels[] = DT_PROP(DATA_NODE, underglow_pixels);

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

void elora_lighting_encode(const struct elora_lighting_state *state, uint8_t sequence,
                           uint32_t *param1, uint32_t *param2) {
    *param1 = state->layers | ((uint32_t)(state->top_layer & 0xf) << P1_TOP_SHIFT) |
              ((uint32_t)sequence << P1_SEQ_SHIFT);
    *param2 = (state->enabled ? P2_ENABLED : 0) | (state->awake ? P2_AWAKE : 0) |
              (state->per_key ? P2_PER_KEY : 0) | ((uint32_t)state->brightness << P2_BRT_SHIFT) |
              ((uint32_t)SNAPSHOT_VERSION << P2_VERSION_SHIFT);
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

/* Underglow color for the requested state, scaled by brightness; 0 when
 * nothing should be lit. */
static uint32_t underglow_color(void) {
    struct elora_lighting_state state;
    bool lit;

    K_SPINLOCK(&lock) {
        state = shown;
        lit = have_state && !suspended;
    }

    if (!lit || !state.enabled || !state.awake || state.top_layer >= ARRAY_SIZE(layer_colors)) {
        return 0;
    }

    uint32_t color = layer_colors[state.top_layer];
    uint32_t percent = MIN(state.brightness, CONFIG_ELORA_LIGHTING_BRT_MAX);
    uint32_t r = ((color >> 16) & 0xff) * percent / 100;
    uint32_t g = ((color >> 8) & 0xff) * percent / 100;
    uint32_t b = (color & 0xff) * percent / 100;

    return (r << 16) | (g << 8) | b;
}

static void fill(uint32_t color) {
    memset(pixels, 0, sizeof(pixels));
    for (size_t i = 0; i < ARRAY_SIZE(underglow_pixels); i++) {
        if (underglow_pixels[i] < NUM_PIXELS) {
            pixels[underglow_pixels[i]] = (struct led_rgb){
                .r = (color >> 16) & 0xff,
                .g = (color >> 8) & 0xff,
                .b = color & 0xff,
            };
        }
    }

    int err = led_strip_update_rgb(strip, pixels, NUM_PIXELS);
    if (err) {
        LOG_ERR("LED strip update failed (%d)", err);
    }
}

static void render_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(render_work, render_work_cb);

static void render_work_cb(struct k_work *work) {
    uint32_t color = underglow_color();

    if (color == 0) {
        if (supply_on) {
            fill(0);
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

    fill(color);
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

    LOG_DBG("snapshot %u: layer %u, enabled %d, awake %d, brightness %u",
            binding->param1 >> P1_SEQ_SHIFT, state.top_layer, state.enabled, state.awake,
            state.brightness);
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
