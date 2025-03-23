/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include "status.h"
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/wpm.h>
#include <zmk/events/position_state_changed.h>
#include "bongocatart.h"
#include <zmk/hid.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/keys.h>

// Add this near the top of the file, after the includes but before any function that uses it
static uint8_t get_current_modifiers(void);

// Add these with the other LV_IMG_DECLARE statements
LV_IMG_DECLARE(control_icon);
LV_IMG_DECLARE(shift_icon);
#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_MAC_MODIFIERS)
LV_IMG_DECLARE(opt_icon);
LV_IMG_DECLARE(cmd_icon);
#else
LV_IMG_DECLARE(alt_icon);
LV_IMG_DECLARE(win_icon);
#endif

// Define wpm_status_state before its first use
struct wpm_status_state {
    uint8_t wpm;                    // Current WPM
    uint8_t wpm_history[10];        // Historical WPM values
    uint8_t animation_state;        // Current animation state
    bool key_pressed;              // Keypress state
    bool is_key_event;            // Flag for key events
    bool is_animation_update;      // Flag for animation updates
};

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

struct layer_status_state {
    uint8_t index;
    const char *label;
};

enum anim_state {
    ANIM_STATE_CASUAL,
    ANIM_STATE_FRENZIED
};
static enum anim_state current_anim_state = ANIM_STATE_CASUAL;

enum idle_anim_state {
    IDLE_INHALE,
    IDLE_REST1,
    IDLE_EXHALE,
    IDLE_REST2
};
static enum idle_anim_state current_idle_state = IDLE_INHALE;

static uint32_t last_idle_update = 0;
static const uint32_t IDLE_ANIMATION_INTERVAL = 750;  // 750ms between idle animation frames

static int32_t breathing_interval_adjustment = 0;
static bool leaving_furious = false;

static uint32_t random_seed = 7919; // Will be initialized with time on first use

static uint32_t last_wpm_update = 0;
static const uint32_t WPM_UPDATE_INTERVAL = 1000;  // Update WPM every 1000ms (1 second)

static struct k_work_delayable animation_work;

static bool key_pressed = false;
static bool key_released = false;
static bool keys_active = false;
static bool use_first_frame = true;
static const lv_img_dsc_t *last_active_frame = &bongo_resting;
static uint32_t last_key_event = 0;
static struct k_work_delayable modifier_work;
static const uint32_t MODIFIER_CHECK_INTERVAL = 20;  // 20ms between checks

static uint32_t last_keypress_time = 0;
static const uint32_t WPM_PAUSE_TIMEOUT = 10000;  // 10 seconds in ms

static uint8_t active_keys = 0;  // Add at top with other static variables

static int32_t get_random_adjustment(void) {
    // Initialize seed with time on first call
    static bool seed_initialized = false;
    if (!seed_initialized) {
        random_seed = random_seed ^ k_uptime_get_32();
        seed_initialized = true;
    }
    
    random_seed = random_seed * 1103515245 + 12345;
    return ((random_seed / 65536) % 501) - 250;
}

LV_IMG_DECLARE(bongocatrest0);
LV_IMG_DECLARE(bongocatcasual1);
LV_IMG_DECLARE(bongocatcasual2);
LV_IMG_DECLARE(bongocatfast1);
LV_IMG_DECLARE(bongocatfast2);
LV_IMG_DECLARE(bongo_resting);
LV_IMG_DECLARE(bongo_casualright);
LV_IMG_DECLARE(bongo_casualleft);
LV_IMG_DECLARE(bongo_furiousup);
LV_IMG_DECLARE(bongo_furiousdown);
LV_IMG_DECLARE(bongo_inhale);
LV_IMG_DECLARE(bongo_exhale);

static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    struct zmk_widget_status *w = (struct zmk_widget_status *)widget;
    lv_obj_t *canvas = w->top_canvas;

    // Clear canvas first
    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);
    
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_label_dsc_t label_dsc_wpm;
    init_label_dsc(&label_dsc_wpm, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_RIGHT);
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);
    lv_draw_line_dsc_t line_dsc;
    init_line_dsc(&line_dsc, LVGL_FOREGROUND, 1);

    // Draw battery
    draw_battery(canvas, state);

    // Draw output status
    char output_text[10] = {};

    switch (state->selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        strcat(output_text, LV_SYMBOL_USB);
        break;
    case ZMK_TRANSPORT_BLE:
        if (state->active_profile_bonded) {
            if (state->active_profile_connected) {
                strcat(output_text, LV_SYMBOL_WIFI);
            } else {
                strcat(output_text, LV_SYMBOL_CLOSE);
            }
        } else {
            strcat(output_text, LV_SYMBOL_SETTINGS);
        }
        break;
    }

    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_dsc, output_text);

    // Draw WPM with smaller rectangle
    #if IS_ENABLED(CONFIG_ZMK_WPM_GRAPH_ENABLED)
    lv_canvas_draw_rect(canvas, 0, 21, 68, 42, &rect_white_dsc);
    lv_canvas_draw_rect(canvas, 1, 22, 66, 40, &rect_black_dsc);

    // Draw BLE profile circle in top left of WPM area
    lv_draw_arc_dsc_t arc_dsc;
    init_arc_dsc(&arc_dsc, LVGL_FOREGROUND, 2);
    lv_draw_arc_dsc_t arc_dsc_filled;
    init_arc_dsc(&arc_dsc_filled, LVGL_FOREGROUND, 9);
    lv_draw_label_dsc_t label_dsc_black;
    init_label_dsc(&label_dsc_black, LVGL_BACKGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);

    int x = 13, y = 34;  // Position circle in top left of WPM area
    lv_canvas_draw_arc(canvas, x, y, 11, 0, 360, &arc_dsc);
    lv_canvas_draw_arc(canvas, x, y, 7, 0, 359, &arc_dsc_filled);

    // Draw BLE profile number
    char label[2];
    snprintf(label, sizeof(label), "%" PRIu8, (uint8_t)(state->active_profile_index + 1));
    lv_canvas_draw_text(canvas, x - 5, y - 10, 10, &label_dsc_black, label);

    // Draw WPM text and graph
    char wpm_text[6] = {};
    snprintf(wpm_text, sizeof(wpm_text), "%d", state->wpm[9]);
    lv_canvas_draw_text(canvas, 42, 52, 24, &label_dsc_wpm, wpm_text);

    // Draw WPM graph
    int max = 0;
    int min = 256;

    for (int i = 0; i < 10; i++) {
        if (state->wpm[i] > max) {
            max = state->wpm[i];
        }
        if (state->wpm[i] < min) {
            min = state->wpm[i];
        }
    }

    int range = max - min;
    if (range == 0) {
        range = 1;
    }

    lv_point_t points[10];
    for (int i = 0; i < 10; i++) {
        points[i].x = 2 + i * 7;
        points[i].y = 60 - (state->wpm[i] - min) * 36 / range;
    }
    lv_canvas_draw_line(canvas, points, 10, &line_dsc);

    // Calculate average WPM and update animation state
    int recent_wpm = 0;
    for (int i = 5; i < 10; i++) {
        recent_wpm += state->wpm[i];
    }
    recent_wpm /= 5;

    // Update animation state based on WPM
    if (recent_wpm > 30) {
        current_anim_state = ANIM_STATE_FRENZIED;
        leaving_furious = false;
    } else if (current_anim_state == ANIM_STATE_FRENZIED) {
        current_anim_state = ANIM_STATE_CASUAL;
        leaving_furious = true;
        current_idle_state = IDLE_EXHALE;
        last_idle_update = k_uptime_get_32();
    }
    #endif

    // No need to rotate - let LVGL handle orientation
    // rotate_canvas(canvas, cbuf);
}

static void draw_middle(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    struct zmk_widget_status *w = (struct zmk_widget_status *)widget;
    lv_obj_t *canvas = w->middle_canvas;

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);
    lv_draw_line_dsc_t line_dsc;
    init_line_dsc(&line_dsc, LVGL_FOREGROUND, 1);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // Draw modifiers at the top
    draw_modifiers(canvas, 0, 13);

    // Draw bongo cat animation frame
    lv_canvas_draw_rect(canvas, 0, 28, 68, 38, &rect_black_dsc);
    
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    
    // Determine which animation frame to use
    const lv_img_dsc_t *current_frame = &bongo_resting;  // Default to resting frame
    
    if (current_anim_state == ANIM_STATE_CASUAL) {
        if (key_pressed) {
            // New key press - alternate animation frames
            if (use_first_frame) {
                last_active_frame = &bongo_casualright;
            } else {
                last_active_frame = &bongo_casualleft;
            }
            use_first_frame = !use_first_frame;
            current_frame = last_active_frame;
        } else if (key_released && !keys_active) {
            // When transitioning to idle, show resting frame first
            current_frame = &bongo_resting;
            last_active_frame = &bongo_resting;
            current_idle_state = IDLE_REST2;  // Start with REST2 so next state will be INHALE
            last_idle_update = k_uptime_get_32() - IDLE_ANIMATION_INTERVAL;  // Force immediate transition
            key_pressed = false;
            key_released = false;
            // Add this to ensure we start breathing animation
            last_key_event = 0;  // Clear the key event time to allow immediate idle transition
        } else if (key_released) {
            // A key was released but others are still active
            current_frame = last_active_frame;
        } else if (keys_active) {
            // Keys are still being held
            current_frame = last_active_frame;
        } else if (k_uptime_get_32() - last_key_event <= MODIFIER_CHECK_INTERVAL * 2) {
            // Short delay after key release, show resting frame
            current_frame = &bongo_resting;
            last_active_frame = &bongo_resting;
        } else {
            // No keys pressed - breathing animation
            switch (current_idle_state) {
                case IDLE_INHALE:
                    current_frame = &bongo_inhale;
                    break;
                case IDLE_REST1:
                    current_frame = &bongo_resting;
                    break;
                case IDLE_EXHALE:
                    current_frame = &bongo_exhale;
                    break;
                case IDLE_REST2:
                    current_frame = &bongo_resting;
                    break;
                default:
                    current_frame = &bongo_resting;
                    break;
            }
        }
    } else { // ANIM_STATE_FRENZIED
        if (key_pressed || key_released) {
            last_active_frame = use_first_frame ? &bongo_furiousup : &bongo_furiousdown;
            use_first_frame = !use_first_frame;
            current_frame = last_active_frame;
        } else if (keys_active) {
            current_frame = last_active_frame;
        } else if (k_uptime_get_32() - last_key_event <= MODIFIER_CHECK_INTERVAL * 2) {
            // Short delay after key release, show last frame
            current_frame = last_active_frame;
        } else {
            // No keys pressed - show breathing animation
            switch (current_idle_state) {
                case IDLE_INHALE:
                    current_frame = &bongo_inhale;
                    break;
                case IDLE_REST1:
                    current_frame = &bongo_resting;
                    break;
                case IDLE_EXHALE:
                    current_frame = &bongo_exhale;
                    break;
                case IDLE_REST2:
                    current_frame = &bongo_resting;
                    break;
                default:
                    current_frame = &bongo_resting;
                    break;
            }
        }
    }

    // Reset key_released flag after handling
    key_released = false;

    // Draw the current animation frame
    lv_canvas_draw_img(canvas, 0, 28, current_frame, &img_dsc);

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

static void draw_bottom(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    struct zmk_widget_status *w = (struct zmk_widget_status *)widget;
    lv_obj_t *canvas = w->bottom_canvas;

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    // Clear this specific canvas only
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // Draw layer text
    if (state->layer_label == NULL) {
        char text[10] = {};
        sprintf(text, "LAYER %i", state->layer_index);
        lv_canvas_draw_text(canvas, 0, 8, 68, &label_dsc, text);
    } else {
        lv_canvas_draw_text(canvas, 0, 8, 68, &label_dsc, state->layer_label);
    }

    rotate_canvas(canvas, cbuf);
}

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

    widget->state.battery = state.level;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_status_state) {
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

static void set_output_status(struct zmk_widget_status *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;

    draw_top(widget->obj, widget->cbuf, &widget->state);  // Only draw the top section
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

static void set_layer_status(struct zmk_widget_status *widget, struct layer_status_state state) {
    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;
    draw_bottom(widget->obj, widget->cbuf3, &widget->state);  // Only update bottom canvas
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    uint8_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){.index = index, .label = zmk_keymap_layer_name(index)};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)

ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

static void process_keypress_event(bool is_pressed, struct zmk_widget_status *widget) {
    key_pressed = is_pressed;
    key_released = !is_pressed;
    
    if (is_pressed) {
        active_keys++;
        keys_active = true;
    } else {
        if (active_keys > 0) active_keys--;
        keys_active = (active_keys > 0);
    }
    
    last_key_event = k_uptime_get_32();
    if (is_pressed) {
        k_work_schedule(&modifier_work, K_MSEC(MODIFIER_CHECK_INTERVAL));
    }
    // Only draw the middle section where modifiers and bongo cat live
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
}

static void modifier_work_handler(struct k_work *work) {
    uint32_t current_time = k_uptime_get_32();
    
    if (!keys_active && (current_time - last_key_event > MODIFIER_CHECK_INTERVAL)) {
        return;
    }
    
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        uint8_t mods = get_current_modifiers();
        if (widget->state.modifiers != mods) {
            widget->state.modifiers = mods;
            for (int i = 0; i < NUM_SYMBOLS; i++) {
                modifier_symbols[i]->is_active = (mods & modifier_symbols[i]->modifier) != 0;
            }
            // Only draw the middle section for modifier updates
            draw_middle(widget->obj, widget->cbuf2, &widget->state);
        }
    }
    
    if (keys_active) {
        k_work_schedule(&modifier_work, K_MSEC(MODIFIER_CHECK_INTERVAL));
    }
}

static void wpm_status_update_cb(struct wpm_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { 
        uint32_t current_time = k_uptime_get_32();
        bool is_animation_update = state.is_animation_update;

        // Update last keypress time if this is a key event
        if (state.is_key_event) {
            last_keypress_time = current_time;
            process_keypress_event(state.key_pressed, widget);
        }

        // Check if we should update WPM
        bool should_update_wpm = false;
        if (!is_animation_update) {
            // Update if enough time has passed since last update
            if (current_time - last_wpm_update >= WPM_UPDATE_INTERVAL) {
                // Only update if either:
                // 1. Less than 10 seconds since last keypress
                // 2. Not all values are zero yet
                bool has_non_zero = false;
                for (int i = 0; i < 10; i++) {
                    if (widget->state.wpm[i] > 0) {
                        has_non_zero = true;
                        break;
                    }
                }
                
                should_update_wpm = (current_time - last_keypress_time < WPM_PAUSE_TIMEOUT) || has_non_zero;
            }
        }

        if (should_update_wpm) {
            // Update WPM array
            for (int i = 0; i < 9; i++) {
                widget->state.wpm[i] = widget->state.wpm[i + 1];
            }
            widget->state.wpm[9] = state.wpm;
            last_wpm_update = current_time;
            
            // Update top display with new WPM data
            draw_top(widget->obj, widget->cbuf, &widget->state);
        }

        // Handle animation updates
        if (is_animation_update) {
            draw_middle(widget->obj, widget->cbuf2, &widget->state);
        }
    }
}

static uint8_t get_current_modifiers(void) {
    uint8_t mods = zmk_hid_get_explicit_mods();
#if IS_ENABLED(CONFIG_ZMK_WIDGET_MODIFIERS_DEBUG)
    LOG_INF("Current mods: %02x", mods);
#endif
    return mods;
}

struct wpm_status_state wpm_status_get_state(const zmk_event_t *eh) {
    static uint8_t wpm_history[10] = {0};
    static uint8_t current_wpm = 0;
    
    const struct zmk_wpm_state_changed *wpm_ev = as_zmk_wpm_state_changed(eh);
    const struct zmk_position_state_changed *pos_ev = as_zmk_position_state_changed(eh);
    const struct zmk_keycode_state_changed *keycode_ev = as_zmk_keycode_state_changed(eh);
    
    bool is_animation_update = false;
    bool is_key_event = false;
    bool key_is_pressed = false;

    // Update WPM if this is a WPM event
    if (wpm_ev != NULL) {
        current_wpm = wpm_ev->state;
        
        if (!is_animation_update) {
            for (int i = 0; i < 9; i++) {
                wpm_history[i] = wpm_history[i + 1];
            }
            wpm_history[9] = current_wpm;
        }
    }

    // Update key state if this is a position event
    if (pos_ev != NULL) {
        is_key_event = true;
        key_is_pressed = pos_ev->state > 0;
    }

    return (struct wpm_status_state){
        .wpm = current_wpm,
        .wpm_history = {wpm_history[0], wpm_history[1], wpm_history[2], wpm_history[3],
                       wpm_history[4], wpm_history[5], wpm_history[6], wpm_history[7],
                       wpm_history[8], wpm_history[9]},
        .animation_state = current_anim_state,
        .key_pressed = key_is_pressed,
        .is_key_event = is_key_event,
        .is_animation_update = is_animation_update
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_wpm_status, struct wpm_status_state, 
                          wpm_status_update_cb, wpm_status_get_state)
ZMK_SUBSCRIPTION(widget_wpm_status, zmk_wpm_state_changed);
ZMK_SUBSCRIPTION(widget_wpm_status, zmk_position_state_changed);
ZMK_SUBSCRIPTION(widget_wpm_status, zmk_keycode_state_changed);

static void animation_work_handler(struct k_work *work) {
    uint32_t current_time = k_uptime_get_32();
    bool needs_redraw = false;
    
    // Check if we should update WPM (every second)
    if (current_time - last_wpm_update >= WPM_UPDATE_INTERVAL) {
        struct zmk_widget_status *widget;
        SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
            // Always update if there are non-zero values or within timeout period
            bool has_non_zero = false;
            for (int i = 0; i < 10; i++) {
                if (widget->state.wpm[i] > 0) {
                    has_non_zero = true;
                    break;
                }
            }

            // Continue updating until all values are zero
            if (has_non_zero || (current_time - last_keypress_time < WPM_PAUSE_TIMEOUT)) {
                // Shift values left
                for (int i = 0; i < 9; i++) {
                    widget->state.wpm[i] = widget->state.wpm[i + 1];
                }
                widget->state.wpm[9] = zmk_wpm_get_state();
                last_wpm_update = current_time;
                
                // Redraw WPM section
                draw_top(widget->obj, widget->cbuf, &widget->state);
            }
        }
    }
    
    // Check if we should update idle animation
    if (current_time - last_idle_update > IDLE_ANIMATION_INTERVAL) {
        last_idle_update = current_time;
        
        // Check for timeout in furious mode
        if (current_anim_state == ANIM_STATE_FRENZIED && 
            (current_time - last_keypress_time > WPM_UPDATE_INTERVAL * 2)) {
            current_anim_state = ANIM_STATE_CASUAL;
            current_idle_state = IDLE_INHALE;
            breathing_interval_adjustment = get_random_adjustment();
            needs_redraw = true;
        }
        
        // Only progress idle animation if we're not actively typing
        if (!keys_active) {  // Remove the last_key_event check to allow immediate transition
            switch (current_idle_state) {
                case IDLE_INHALE:
                    current_idle_state = IDLE_REST1;
                    needs_redraw = true;
                    break;
                case IDLE_REST1:
                    current_idle_state = IDLE_EXHALE;
                    needs_redraw = true;
                    break;
                case IDLE_EXHALE:
                    current_idle_state = IDLE_REST2;
                    if (leaving_furious) {
                        leaving_furious = false;
                    }
                    needs_redraw = true;
                    break;
                case IDLE_REST2:
                    current_idle_state = IDLE_INHALE;
                    if (!leaving_furious) {
                        breathing_interval_adjustment = get_random_adjustment();
                    }
                    needs_redraw = true;
                    break;
            }
        }
        
        // If animation state changed, trigger a redraw
        if (needs_redraw) {
            struct zmk_widget_status *widget;
            SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
                draw_middle(widget->obj, widget->cbuf2, &widget->state);
            }
        }
    }
    
    // Schedule next check
    uint32_t next_check = MIN(WPM_UPDATE_INTERVAL / 4, IDLE_ANIMATION_INTERVAL / 2);
    k_work_schedule(&animation_work, K_MSEC(next_check));
}

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);  // Full widget size is 160x68
    
    // Create top canvas with correct size
    widget->top_canvas = lv_canvas_create(widget->obj);
    lv_obj_align(widget->top_canvas, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(widget->top_canvas, widget->cbuf, 68, 68, LV_IMG_CF_TRUE_COLOR);  // Set actual dimensions
    
    // Create middle canvas with correct size
    widget->middle_canvas = lv_canvas_create(widget->obj);
    lv_obj_align(widget->middle_canvas, LV_ALIGN_CENTER, 24, 0);
    lv_canvas_set_buffer(widget->middle_canvas, widget->cbuf2, 68, 68, LV_IMG_CF_TRUE_COLOR);
    
    // Create bottom canvas with correct size
    widget->bottom_canvas = lv_canvas_create(widget->obj);
    lv_obj_align(widget->bottom_canvas, LV_ALIGN_BOTTOM_LEFT, -44, 0);
    lv_canvas_set_buffer(widget->bottom_canvas, widget->cbuf3, 68, 68, LV_IMG_CF_TRUE_COLOR);

    // Set canvas background colors
    lv_canvas_fill_bg(widget->top_canvas, LVGL_BACKGROUND, LV_OPA_COVER);
    lv_canvas_fill_bg(widget->middle_canvas, LVGL_BACKGROUND, LV_OPA_COVER);
    lv_canvas_fill_bg(widget->bottom_canvas, LVGL_BACKGROUND, LV_OPA_COVER);

    sys_slist_append(&widgets, &widget->node);
    
    // Initialize widget listeners
    widget_battery_status_init();
    widget_output_status_init();
    widget_layer_status_init();
    widget_wpm_status_init();

    // Initialize animation worker
    k_work_init_delayable(&animation_work, animation_work_handler);
    k_work_init_delayable(&modifier_work, modifier_work_handler);
    k_work_schedule(&animation_work, K_MSEC(IDLE_ANIMATION_INTERVAL));

    // Initialize modifier states to inactive
    for (int i = 0; i < NUM_SYMBOLS; i++) {
        modifier_symbols[i]->is_active = false;
    }
    widget->state.modifiers = 0;
    
    // Force initial draw of all sections
    draw_top(widget->obj, widget->cbuf, &widget->state);
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
    draw_bottom(widget->obj, widget->cbuf3, &widget->state);

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }


