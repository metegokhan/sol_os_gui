/*
 * Solar OS - BLE & System Mouse Service
 * Virtual Cursor & Navigation Engine for ST7305 RLCD (400x300)
 */

#include "solar_os_mouse.h"

#include <math.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"

#define TAG "mouse"

static solar_os_mouse_state_t mouse_state = {
    .x = SOLAR_OS_MOUSE_WIDTH / 2,
    .y = SOLAR_OS_MOUSE_HEIGHT / 2,
    .buttons = 0,
    .wheel = 0,
    .visible = false,
    .connected = false,
    .last_active_ms = 0,
};

static uint8_t prev_buttons = 0;
static solar_os_input_source_t mouse_input_source = SOLAR_OS_INPUT_SOURCE_INVALID;

static portMUX_TYPE mouse_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t mouse_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

esp_err_t solar_os_mouse_init(void)
{
    portENTER_CRITICAL(&mouse_lock);
    mouse_state.x = SOLAR_OS_MOUSE_WIDTH / 2;
    mouse_state.y = SOLAR_OS_MOUSE_HEIGHT / 2;
    mouse_state.buttons = 0;
    mouse_state.wheel = 0;
    mouse_state.visible = false;
    mouse_state.connected = false;
    mouse_state.last_active_ms = 0;
    prev_buttons = 0;
    portEXIT_CRITICAL(&mouse_lock);

    if (mouse_input_source == SOLAR_OS_INPUT_SOURCE_INVALID) {
        (void)solar_os_input_source_open("ble-mouse", &mouse_input_source);
    }

    SOLAR_OS_LOGI(TAG, "Mouse service initialized");
    return ESP_OK;
}

void solar_os_mouse_set_connected(bool connected)
{
    portENTER_CRITICAL(&mouse_lock);
    mouse_state.connected = connected;
    if (!connected) {
        mouse_state.visible = false;
    }
    portEXIT_CRITICAL(&mouse_lock);
}

bool solar_os_mouse_is_connected(void)
{
    bool connected;
    portENTER_CRITICAL(&mouse_lock);
    connected = mouse_state.connected;
    portEXIT_CRITICAL(&mouse_lock);
    return connected;
}

void solar_os_mouse_process_report(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel)
{
    const uint32_t now = mouse_millis();

    portENTER_CRITICAL(&mouse_lock);

    /* Update coordinates with smooth sub-pixel motion divider (4x slower/smoother) */
    static float s_cursor_fx = SOLAR_OS_MOUSE_WIDTH / 2.0f;
    static float s_cursor_fy = SOLAR_OS_MOUSE_HEIGHT / 2.0f;
    const float SPEED_DIVIDER = 4.0f;

    s_cursor_fx += ((float)dx / SPEED_DIVIDER);
    s_cursor_fy += ((float)dy / SPEED_DIVIDER);

    if (s_cursor_fx < 0.0f) s_cursor_fx = 0.0f;
    if (s_cursor_fx >= (float)SOLAR_OS_MOUSE_WIDTH) s_cursor_fx = (float)(SOLAR_OS_MOUSE_WIDTH - 1);
    if (s_cursor_fy < 0.0f) s_cursor_fy = 0.0f;
    if (s_cursor_fy >= (float)SOLAR_OS_MOUSE_HEIGHT) s_cursor_fy = (float)(SOLAR_OS_MOUSE_HEIGHT - 1);

    mouse_state.x = (int16_t)s_cursor_fx;
    mouse_state.y = (int16_t)s_cursor_fy;
    mouse_state.buttons = buttons;
    mouse_state.wheel = wheel;
    mouse_state.visible = true;
    mouse_state.connected = true;
    mouse_state.last_active_ms = now;

    portEXIT_CRITICAL(&mouse_lock);

    /* 1. Button State Translation */
    /* Left Button (Bit 0): Enter / Select */
    const bool left_pressed = (buttons & SOLAR_OS_MOUSE_BTN_LEFT) != 0;
    const bool left_prev = (prev_buttons & SOLAR_OS_MOUSE_BTN_LEFT) != 0;
    if (left_pressed && !left_prev && mouse_input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        (void)solar_os_input_write_key(mouse_input_source,
                                       0xE001,
                                       SOLAR_OS_INPUT_USAGE_NONE,
                                       SOLAR_OS_KEY_ENTER,
                                       0,
                                       SOLAR_OS_INPUT_KEY_PRESS);
    } else if (!left_pressed && left_prev && mouse_input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        (void)solar_os_input_write_key(mouse_input_source,
                                       0xE001,
                                       SOLAR_OS_INPUT_USAGE_NONE,
                                       SOLAR_OS_KEY_ENTER,
                                       0,
                                       SOLAR_OS_INPUT_KEY_RELEASE);
    }

    /* Right Button (Bit 1): ESC / Back */
    const bool right_pressed = (buttons & SOLAR_OS_MOUSE_BTN_RIGHT) != 0;
    const bool right_prev = (prev_buttons & SOLAR_OS_MOUSE_BTN_RIGHT) != 0;
    if (right_pressed && !right_prev && mouse_input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        (void)solar_os_input_write_key(mouse_input_source,
                                       0xE002,
                                       SOLAR_OS_INPUT_USAGE_NONE,
                                       SOLAR_OS_KEY_ESCAPE,
                                       0,
                                       SOLAR_OS_INPUT_KEY_PRESS);
    } else if (!right_pressed && right_prev && mouse_input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        (void)solar_os_input_write_key(mouse_input_source,
                                       0xE002,
                                       SOLAR_OS_INPUT_USAGE_NONE,
                                       SOLAR_OS_KEY_ESCAPE,
                                       0,
                                       SOLAR_OS_INPUT_KEY_RELEASE);
    }

    /* Middle Button (Bit 2): Tab */
    const bool mid_pressed = (buttons & SOLAR_OS_MOUSE_BTN_MIDDLE) != 0;
    const bool mid_prev = (prev_buttons & SOLAR_OS_MOUSE_BTN_MIDDLE) != 0;
    if (mid_pressed && !mid_prev && mouse_input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        (void)solar_os_input_write_key(mouse_input_source,
                                       0xE003,
                                       SOLAR_OS_INPUT_USAGE_NONE,
                                       '\t',
                                       0,
                                       SOLAR_OS_INPUT_KEY_PRESS);
    } else if (!mid_pressed && mid_prev && mouse_input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        (void)solar_os_input_write_key(mouse_input_source,
                                       0xE003,
                                       SOLAR_OS_INPUT_USAGE_NONE,
                                       '\t',
                                       0,
                                       SOLAR_OS_INPUT_KEY_RELEASE);
    }

    prev_buttons = buttons;
}

void solar_os_mouse_get_state(solar_os_mouse_state_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&mouse_lock);
    *out = mouse_state;
    portEXIT_CRITICAL(&mouse_lock);
}

bool solar_os_mouse_is_visible(void)
{
    bool visible;
    portENTER_CRITICAL(&mouse_lock);
    visible = mouse_state.visible;
    portEXIT_CRITICAL(&mouse_lock);
    return visible;
}

void solar_os_mouse_tick(uint32_t now_ms)
{
    portENTER_CRITICAL(&mouse_lock);
    if (mouse_state.visible && (now_ms - mouse_state.last_active_ms) > SOLAR_OS_MOUSE_TIMEOUT_MS) {
        mouse_state.visible = false;
    }
    portEXIT_CRITICAL(&mouse_lock);
}

/* ---------------------------------------------------------------------
 * Software Cursor Overlay Drawing (Arrow Sprite for ST7305 RLCD)
 * ------------------------------------------------------------------- */
void solar_os_mouse_draw_cursor(solar_os_gfx_t *gfx)
{
    if (gfx == NULL) return;

    int mx, my;
    bool vis;
    portENTER_CRITICAL(&mouse_lock);
    mx = mouse_state.x;
    my = mouse_state.y;
    vis = mouse_state.visible;
    portEXIT_CRITICAL(&mouse_lock);

    if (!vis) return;

    /*
     * Classic 11x14 arrow cursor bitmask
     * 1 = Black outline, 2 = White interior
     */
    static const uint8_t cursor_mask[14] = {
        0b10000000,
        0b11000000,
        0b11100000,
        0b11110000,
        0b11111000,
        0b11111100,
        0b11111110,
        0b11111111,
        0b11111000,
        0b11011100,
        0b10001100,
        0b00001110,
        0b00000110,
        0b00000000,
    };

    static const uint8_t cursor_fill[14] = {
        0b00000000,
        0b01000000,
        0b01100000,
        0b01110000,
        0b01111000,
        0b01111100,
        0b01111100,
        0b01111000,
        0b01100000,
        0b01001100,
        0b00000110,
        0b00000110,
        0b00000000,
        0b00000000,
    };

    /* Draw white interior fill */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    for (int r = 0; r < 14; r++) {
        uint8_t row = cursor_fill[r];
        for (int c = 0; c < 8; c++) {
            if (row & (1 << (7 - c))) {
                solar_os_gfx_pixel(gfx, mx + c, my + r);
            }
        }
    }

    /* Draw black crisp outline */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    for (int r = 0; r < 14; r++) {
        uint8_t row = cursor_mask[r];
        for (int c = 0; c < 8; c++) {
            if ((row & (1 << (7 - c))) && !(cursor_fill[r] & (1 << (7 - c)))) {
                solar_os_gfx_pixel(gfx, mx + c, my + r);
            }
        }
    }
}
