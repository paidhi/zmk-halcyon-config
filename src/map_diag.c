/*
 * SPDX-License-Identifier: MIT
 *
 * LED chain mapping diagnostic.
 *
 * Lights one pixel of the zmk,underglow chain at a time. A tap records the
 * tapped key's position for the lit pixel, a hold of 1-3 s records that the
 * pixel is not under a key, and a hold of 3 s or more steps back one pixel.
 * Every key press is consumed, so nothing reaches the keymap, the host, or
 * the other half. After the last pixel, a rail check switches the LED supply
 * off and on while the central's display changes layer, then the result
 * summary repeats in the log until the half is reset.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/keymap.h>
#endif

LOG_MODULE_REGISTER(elora_map_diag, LOG_LEVEL_INF);

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)
#define SUPPLY_NODE DT_INST(0, zmk_ext_power_generic)

#define HOLD_MS 1000
#define UNDO_MS 3000
#define START_DELAY_MS 5000
#define SUPPLY_SETTLE_MS 100
#define RAIL_STEP_MS 5000
#define RAIL_STEPS 5
#define SUMMARY_PERIOD_MS 10000
#define SUMMARY_PER_LINE 8

#define RECORD_UNSET -2
#define RECORD_NOT_A_KEY -1

enum action_type { ACTION_TAP, ACTION_SKIP, ACTION_UNDO };

struct key_action {
    enum action_type type;
    uint32_t position;
    uint8_t source;
};

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static const struct gpio_dt_spec supply = GPIO_DT_SPEC_GET_BY_IDX(SUPPLY_NODE, control_gpios, 0);

static struct led_rgb pixels[NUM_PIXELS];
static int16_t records[NUM_PIXELS];
static int current;
static int rail_step;
static bool rail_checked;

/* Press tracking runs in the event listener; everything else on the system
 * work queue, which serializes access to the mapping state. */
static uint32_t pressed_position = UINT32_MAX;
static int64_t pressed_at;

K_MSGQ_DEFINE(actions, sizeof(struct key_action), 8, 4);

static void show(int lit_pixel, bool all) {
    for (int i = 0; i < NUM_PIXELS; i++) {
        uint8_t level = (all || i == lit_pixel) ? CONFIG_ELORA_LIGHTING_MAP_DIAG_LEVEL : 0;
        pixels[i] = (struct led_rgb){.r = level, .g = level, .b = level};
    }

    int err = led_strip_update_rgb(strip, pixels, NUM_PIXELS);
    if (err) {
        LOG_ERR("strip update failed (%d)", err);
    }
}

static void set_supply(bool on) {
    gpio_pin_set_dt(&supply, on);
    if (on) {
        k_msleep(SUPPLY_SETTLE_MS);
    }
}

static void ask(void) {
    show(current, false);
    LOG_INF("ask pixel=%d", current);
}

static void log_summary(void) {
    LOG_INF("summary begin pixels=%d (x = not a key, ? = unset)", NUM_PIXELS);
    for (int first = 0; first < NUM_PIXELS; first += SUMMARY_PER_LINE) {
        char line[SUMMARY_PER_LINE * 4 + 1];
        size_t len = 0;

        for (int i = first; i < MIN(first + SUMMARY_PER_LINE, NUM_PIXELS); i++) {
            if (records[i] >= 0) {
                len += snprintk(line + len, sizeof(line) - len, " %d", records[i]);
            } else {
                len += snprintk(line + len, sizeof(line) - len, " %c",
                                records[i] == RECORD_NOT_A_KEY ? 'x' : '?');
            }
        }
        LOG_INF("summary pixels %d-%d:%s", first,
                MIN(first + SUMMARY_PER_LINE, NUM_PIXELS) - 1, line);
    }
    LOG_INF("summary end");
}

static void summary_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(summary_work, summary_work_cb);

static void summary_work_cb(struct k_work *work) {
    log_summary();
    k_work_schedule(&summary_work, K_MSEC(SUMMARY_PERIOD_MS));
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void set_top_layer(bool active) {
    zmk_keymap_layer_id_t top = zmk_keymap_layer_index_to_id(ZMK_KEYMAP_LAYERS_LEN - 1);

    if (active) {
        zmk_keymap_layer_activate(top, false);
    } else {
        zmk_keymap_layer_deactivate(top, false);
    }
}
#else
static void set_top_layer(bool active) { ARG_UNUSED(active); }
#endif

static void rail_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(rail_work, rail_work_cb);

/* Odd steps cut the supply while the central shows its top layer, so the
 * e-paper must refresh without the LED supply. Even steps restore the
 * supply with every pixel dim, which shows that switching works. */
static void rail_work_cb(struct k_work *work) {
    rail_step++;

    if (rail_step < RAIL_STEPS) {
        bool supply_on = (rail_step % 2) == 0;

        set_supply(supply_on);
        show(-1, supply_on);
        set_top_layer(!supply_on);
        LOG_INF("rail-check step %d/%d: LED supply %s, all LEDs %s, central display %s",
                rail_step, RAIL_STEPS - 1, supply_on ? "on" : "off", supply_on ? "dim" : "dark",
                supply_on ? "back to the base layer" : "should show the top layer");
        k_work_schedule(&rail_work, K_MSEC(RAIL_STEP_MS));
        return;
    }

    show(-1, false);
    set_supply(false);
    rail_checked = true;
    LOG_INF("rail-check done: LED supply off");
    k_work_schedule(&summary_work, K_NO_WAIT);
}

static void finish(void) {
    show(-1, false);
    LOG_INF("all %d pixels recorded", NUM_PIXELS);
    log_summary();

    if (rail_checked) {
        set_supply(false);
        k_work_schedule(&summary_work, K_MSEC(SUMMARY_PERIOD_MS));
    } else {
        rail_step = 0;
        LOG_INF("rail-check starts; watch the LEDs and the central's e-paper");
        k_work_schedule(&rail_work, K_MSEC(RAIL_STEP_MS));
    }
}

static void undo(void) {
    if (current == 0) {
        LOG_INF("undo ignored at pixel 0");
        return;
    }

    if (current == NUM_PIXELS) {
        struct k_work_sync sync;

        k_work_cancel_delayable_sync(&rail_work, &sync);
        k_work_cancel_delayable_sync(&summary_work, &sync);
        set_top_layer(false);
        set_supply(true);
    }

    current--;
    records[current] = RECORD_UNSET;
    LOG_INF("undo: back to pixel=%d", current);
    ask();
}

static void handle(const struct key_action *action) {
    if (action->type == ACTION_UNDO) {
        undo();
        return;
    }

    if (current == NUM_PIXELS) {
        LOG_INF("all pixels recorded; hold a key 3 s or more to step back");
        return;
    }

    if (action->type == ACTION_TAP) {
        records[current] = action->position;
        LOG_INF("record pixel=%d position=%d source=%d", current, action->position,
                action->source);
    } else {
        records[current] = RECORD_NOT_A_KEY;
        LOG_INF("record pixel=%d not-a-key", current);
    }

    current++;
    if (current < NUM_PIXELS) {
        ask();
    } else {
        finish();
    }
}

static void action_work_cb(struct k_work *work) {
    struct key_action action;

    while (k_msgq_get(&actions, &action, K_NO_WAIT) == 0) {
        handle(&action);
    }
}

static K_WORK_DEFINE(action_work, action_work_cb);

static void start_work_cb(struct k_work *work) {
    LOG_INF("start: %s, %d pixels, level %d",
            IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) ? "central" : "peripheral", NUM_PIXELS,
            CONFIG_ELORA_LIGHTING_MAP_DIAG_LEVEL);
    LOG_INF("tap the key under the lit pixel; hold 1-3 s if it is not under a key; "
            "hold 3 s or more to step back");
    set_supply(true);
    ask();
}

static K_WORK_DELAYABLE_DEFINE(start_work, start_work_cb);

static int map_diag_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        pressed_position = ev->position;
        pressed_at = ev->timestamp;
    } else if (ev->position == pressed_position) {
        int64_t held = ev->timestamp - pressed_at;
        struct key_action action = {
            .type = held >= UNDO_MS   ? ACTION_UNDO
                    : held >= HOLD_MS ? ACTION_SKIP
                                      : ACTION_TAP,
            .position = ev->position,
            .source = ev->source,
        };

        pressed_position = UINT32_MAX;
        if (k_msgq_put(&actions, &action, K_NO_WAIT) == 0) {
            k_work_submit(&action_work);
        } else {
            LOG_WRN("key action dropped; press again");
        }
    }

    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(elora_map_diag, map_diag_listener);
ZMK_SUBSCRIPTION(elora_map_diag, zmk_position_state_changed);

static int map_diag_init(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&supply) || gpio_pin_configure_dt(&supply, GPIO_OUTPUT_INACTIVE)) {
        LOG_ERR("LED supply pin not ready");
        return -EIO;
    }

    for (int i = 0; i < NUM_PIXELS; i++) {
        records[i] = RECORD_UNSET;
    }

    /* Leave time to attach the log console before the first pixel. */
    k_work_schedule(&start_work, K_MSEC(START_DELAY_MS));
    return 0;
}

SYS_INIT(map_diag_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
