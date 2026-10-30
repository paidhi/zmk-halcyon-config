/*
 * SPDX-License-Identifier: MIT
 *
 * Layer lighting state on the central.
 *
 * Tracks the highest active layer, the user's on/off and brightness settings,
 * and a lighting idle timer fed by key presses from both halves. Every change
 * renders locally and sends one complete snapshot to each peripheral through
 * the lit_sync behavior. The split link can drop a command while a
 * peripheral is still being discovered, so a new split connection triggers a
 * few spaced resends. The &lit behavior handles the keymap controls.
 */

#define DT_DRV_COMPAT elora_behavior_lighting

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <drivers/behavior.h>
#include <dt-bindings/elora/lighting.h>
#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/split/central.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
#include <zephyr/bluetooth/conn.h>
#endif

#include "lighting.h"

LOG_MODULE_DECLARE(elora_lighting, CONFIG_ZMK_LOG_LEVEL);

#define DATA_NODE DT_INST(0, elora_lighting)
#define SYNC_NODE DT_INST(0, elora_behavior_lighting_sync)

BUILD_ASSERT(DT_PROP_LEN(DATA_NODE, layer_colors) == ZMK_KEYMAP_LAYERS_LEN,
             "elora_lighting layer-colors needs one entry per keymap layer");
BUILD_ASSERT(ZMK_KEYMAP_LAYERS_LEN <= 16, "the snapshot carries at most 16 layers");
BUILD_ASSERT(CONFIG_ELORA_LIGHTING_BRT_MIN <= CONFIG_ELORA_LIGHTING_BRT_START &&
                 CONFIG_ELORA_LIGHTING_BRT_START <= CONFIG_ELORA_LIGHTING_BRT_MAX,
             "lighting start brightness must lie within the min/max range");

#define RESEND_COUNT 4

struct lighting_settings {
    uint8_t enabled;
    uint8_t brightness;
};

static struct lighting_settings settings = {
    .enabled = 1,
    .brightness = CONFIG_ELORA_LIGHTING_BRT_START,
};
static atomic_t awake = ATOMIC_INIT(1);
static uint8_t sequence;

static void publish_work_cb(struct k_work *work) {
    struct elora_lighting_state state = {
        .layers = (uint16_t)(zmk_keymap_layer_state() | BIT(zmk_keymap_layer_default())),
        .top_layer = zmk_keymap_layer_index_to_id(zmk_keymap_highest_layer_active()),
        .enabled = settings.enabled,
        .awake = atomic_get(&awake),
        .per_key = false,
        .brightness = settings.brightness,
    };

    elora_lighting_show(&state);

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    struct zmk_behavior_binding binding = {.behavior_dev = DEVICE_DT_NAME(SYNC_NODE)};
    struct zmk_behavior_binding_event event = {.timestamp = k_uptime_get()};

    elora_lighting_encode(&state, sequence++, &binding.param1, &binding.param2);
    for (uint8_t source = 0; source < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; source++) {
        int err = zmk_split_central_invoke_behavior(source, &binding, event, true);
        if (err) {
            LOG_DBG("snapshot to peripheral %u not queued (%d)", source, err);
        }
    }
#endif
}

static K_WORK_DEFINE(publish_work, publish_work_cb);

static void publish(void) { k_work_submit(&publish_work); }

static void idle_work_cb(struct k_work *work) {
    atomic_set(&awake, 0);
    publish();
}

static K_WORK_DELAYABLE_DEFINE(idle_work, idle_work_cb);

static void note_activity(void) {
    k_work_reschedule(&idle_work, K_MSEC(CONFIG_ELORA_LIGHTING_IDLE_TIMEOUT_MS));
    if (!atomic_set(&awake, 1)) {
        publish();
    }
}

static int central_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        note_activity();
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (as_zmk_layer_state_changed(eh) != NULL) {
        note_activity();
        publish();
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_activity_state_changed *activity = as_zmk_activity_state_changed(eh);
    if (activity != NULL && activity->state != ZMK_ACTIVITY_ACTIVE) {
        k_work_cancel_delayable(&idle_work);
        atomic_set(&awake, 0);
        publish();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(elora_lighting_central, central_listener);
ZMK_SUBSCRIPTION(elora_lighting_central, zmk_position_state_changed);
ZMK_SUBSCRIPTION(elora_lighting_central, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(elora_lighting_central, zmk_activity_state_changed);

#if IS_ENABLED(CONFIG_SETTINGS)
static void save_work_cb(struct k_work *work) {
    int err = settings_save_one("elora_lit/state", &settings, sizeof(settings));
    if (err) {
        LOG_ERR("saving lighting settings failed (%d)", err);
    }
}

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_cb);

static void save_settings(void) {
    k_work_reschedule(&save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
}

static int settings_set_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;

    if (!settings_name_steq(name, "state", &next) || next) {
        return -ENOENT;
    }

    struct lighting_settings stored;
    if (len != sizeof(stored) || read_cb(cb_arg, &stored, sizeof(stored)) != sizeof(stored)) {
        return -EINVAL;
    }

    settings.enabled = stored.enabled ? 1 : 0;
    settings.brightness =
        CLAMP(stored.brightness, CONFIG_ELORA_LIGHTING_BRT_MIN, CONFIG_ELORA_LIGHTING_BRT_MAX);
    return 0;
}

static int settings_commit_cb(void) {
    publish();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(elora_lighting, "elora_lit", NULL, settings_set_cb,
                               settings_commit_cb, NULL);
#else
static void save_settings(void) {}
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
static int resend_step;

static void resend_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(resend_work, resend_work_cb);

/* Resends after about 1, 3, 7, and 15 s cover the peripheral's discovery. */
static void resend_work_cb(struct k_work *work) {
    publish();
    if (++resend_step < RESEND_COUNT) {
        k_work_schedule(&resend_work, K_SECONDS(1 << resend_step));
    }
}

static void split_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;

    /* Host links have this half in the peripheral role; split links do not. */
    if (err || bt_conn_get_info(conn, &info) || info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }

    resend_step = 0;
    k_work_reschedule(&resend_work, K_SECONDS(1));
}

BT_CONN_CB_DEFINE(elora_lighting_conn_cb) = {
    .connected = split_connected,
};
#endif

static int central_init(void) {
    k_work_schedule(&idle_work, K_MSEC(CONFIG_ELORA_LIGHTING_IDLE_TIMEOUT_MS));
    publish();
    return 0;
}

SYS_INIT(central_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* &lit: keymap controls. */
static int lit_pressed(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case LIT_ON:
        settings.enabled = 1;
        break;
    case LIT_OFF:
        settings.enabled = 0;
        break;
    case LIT_BRI:
        settings.brightness = MIN(settings.brightness + CONFIG_ELORA_LIGHTING_BRT_STEP,
                                  CONFIG_ELORA_LIGHTING_BRT_MAX);
        break;
    case LIT_BRD:
        settings.brightness = MAX(settings.brightness - CONFIG_ELORA_LIGHTING_BRT_STEP,
                                  CONFIG_ELORA_LIGHTING_BRT_MIN);
        break;
    default:
        return -ENOTSUP;
    }

    save_settings();
    publish();
    return ZMK_BEHAVIOR_OPAQUE;
}

static int lit_released(struct zmk_behavior_binding *binding,
                        struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api lit_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = lit_pressed,
    .binding_released = lit_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &lit_driver_api);
