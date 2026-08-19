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

#include "solar_os_display.h"
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
static float cursor_fx = SOLAR_OS_MOUSE_WIDTH / 2.0f;
static float cursor_fy = SOLAR_OS_MOUSE_HEIGHT / 2.0f;

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
    cursor_fx = SOLAR_OS_MOUSE_WIDTH / 2.0f;
    cursor_fy = SOLAR_OS_MOUSE_HEIGHT / 2.0f;
    portEXIT_CRITICAL(&mouse_lock);

    solar_os_mouse_compositor_invalidate();

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

static volatile bool mouse_suppressed = false;

void solar_os_mouse_set_suppressed(bool suppressed)
{
    mouse_suppressed = suppressed;
}

bool solar_os_mouse_is_suppressed(void)
{
    return mouse_suppressed;
}

bool solar_os_mouse_is_connected(void)
{
    bool connected;
    portENTER_CRITICAL(&mouse_lock);
    connected = mouse_state.connected;
    portEXIT_CRITICAL(&mouse_lock);
    return connected;
}

static bool mouse_dirty = false;

/* Pending click: the (x,y) the cursor was at on the left-button press edge,
 * for apps that want to hit-test their own on-screen elements instead of
 * (or in addition to) the generic Enter-key emulation below. Popped once by
 * main.c's dispatch loop -- see solar_os_mouse_take_pending_click(). */
static bool click_pending = false;
static int16_t click_pending_x = 0;
static int16_t click_pending_y = 0;
static uint8_t click_pending_buttons = 0;

/* Pending scroll: wheel notches accumulated since the last pop, plus the
 * cursor position at the time of accumulation. Popped once by main.c's
 * dispatch loop -- see solar_os_mouse_take_pending_scroll(). */
static int16_t scroll_pending_delta = 0;
static int16_t scroll_pending_x = 0;
static int16_t scroll_pending_y = 0;

/* Pending drag step: accumulated cursor delta while the left button is
 * held, plus the press/release edge flags. Popped once by main.c's
 * dispatch loop -- see solar_os_mouse_take_pending_drag(). `started`
 * stays true across accumulation until popped, so a caller that hasn't
 * polled since the press still sees it on the first pop. */
static bool drag_pending = false;
static int16_t drag_pending_x = 0;
static int16_t drag_pending_y = 0;
static int16_t drag_pending_dx = 0;
static int16_t drag_pending_dy = 0;
static uint8_t drag_pending_buttons = 0;
static bool drag_pending_started = false;
static bool drag_pending_ended = false;

void solar_os_mouse_process_report(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel)
{
    if (mouse_suppressed) {
        return;
    }
    const uint32_t now = mouse_millis();
    int16_t final_x, final_y;

    portENTER_CRITICAL(&mouse_lock);

    if (dx != 0 || dy != 0) {
        cursor_fx += (float)dx;
        cursor_fy += (float)dy;

        if (cursor_fx < 0.0f) cursor_fx = 0.0f;
        if (cursor_fx >= (float)SOLAR_OS_MOUSE_WIDTH) cursor_fx = (float)(SOLAR_OS_MOUSE_WIDTH - 1);
        if (cursor_fy < 0.0f) cursor_fy = 0.0f;
        if (cursor_fy >= (float)SOLAR_OS_MOUSE_HEIGHT) cursor_fy = (float)(SOLAR_OS_MOUSE_HEIGHT - 1);

        mouse_state.x = (int16_t)cursor_fx;
        mouse_state.y = (int16_t)cursor_fy;
        mouse_dirty = true;
    }

    mouse_state.buttons = buttons;
    mouse_state.wheel = wheel;
    mouse_state.visible = true;
    mouse_state.connected = true;
    mouse_state.last_active_ms = now;
    final_x = mouse_state.x;
    final_y = mouse_state.y;

    if (wheel != 0) {
        scroll_pending_delta += wheel;
        scroll_pending_x = final_x;
        scroll_pending_y = final_y;
    }

    portEXIT_CRITICAL(&mouse_lock);

    /* 1. Button State Translation */
    /* Left Button (Bit 0): positional click only (SOLAR_OS_EVENT_CLICK).
     * This used to also synthesize a global Enter keypress, but that meant
     * every left-click fired TWO actions: Enter first (activating whatever
     * item was already selected from prior keyboard/click nav), then the
     * click landing wherever the cursor actually was -- e.g. clicking one
     * folder over from the selected one would open the OLD selection and
     * then act on the new one. Position is now the only signal; apps that
     * don't handle SOLAR_OS_EVENT_CLICK simply don't get left-click input,
     * same as before this mouse service existed. */
    const bool left_pressed = (buttons & SOLAR_OS_MOUSE_BTN_LEFT) != 0;
    const bool left_prev = (prev_buttons & SOLAR_OS_MOUSE_BTN_LEFT) != 0;
    if (left_pressed && !left_prev) {
        portENTER_CRITICAL(&mouse_lock);
        click_pending = true;
        click_pending_x = final_x;
        click_pending_y = final_y;
        click_pending_buttons = SOLAR_OS_MOUSE_BTN_LEFT;
        portEXIT_CRITICAL(&mouse_lock);
    }

    /* Drag: press edge, held+move steps, and release edge all feed one
     * accumulating pending record so apps with a drag-to-adjust control
     * (e.g. a synth knob) see every step even if dispatch polls slower
     * than incoming HID reports. */
    if (left_pressed && !left_prev) {
        portENTER_CRITICAL(&mouse_lock);
        drag_pending = true;
        drag_pending_x = final_x;
        drag_pending_y = final_y;
        drag_pending_dx = 0;
        drag_pending_dy = 0;
        drag_pending_buttons = buttons;
        drag_pending_started = true;
        drag_pending_ended = false;
        portEXIT_CRITICAL(&mouse_lock);
    } else if (left_pressed && left_prev && (dx != 0 || dy != 0)) {
        portENTER_CRITICAL(&mouse_lock);
        drag_pending = true;
        drag_pending_x = final_x;
        drag_pending_y = final_y;
        drag_pending_dx = (int16_t)(drag_pending_dx + dx);
        drag_pending_dy = (int16_t)(drag_pending_dy + dy);
        drag_pending_buttons = buttons;
        drag_pending_ended = false;
        portEXIT_CRITICAL(&mouse_lock);
    } else if (!left_pressed && left_prev) {
        portENTER_CRITICAL(&mouse_lock);
        drag_pending = true;
        drag_pending_x = final_x;
        drag_pending_y = final_y;
        drag_pending_dx = 0;
        drag_pending_dy = 0;
        drag_pending_buttons = buttons;
        drag_pending_ended = true;
        portEXIT_CRITICAL(&mouse_lock);
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

bool solar_os_mouse_is_dirty(void)
{
    bool dirty;
    portENTER_CRITICAL(&mouse_lock);
    dirty = mouse_dirty;
    portEXIT_CRITICAL(&mouse_lock);
    return dirty;
}

void solar_os_mouse_clear_dirty(void)
{
    portENTER_CRITICAL(&mouse_lock);
    mouse_dirty = false;
    portEXIT_CRITICAL(&mouse_lock);
}

bool solar_os_mouse_take_pending_click(int16_t *x, int16_t *y, uint8_t *buttons)
{
    bool had_click;
    portENTER_CRITICAL(&mouse_lock);
    had_click = click_pending;
    if (had_click) {
        if (x != NULL) *x = click_pending_x;
        if (y != NULL) *y = click_pending_y;
        if (buttons != NULL) *buttons = click_pending_buttons;
        click_pending = false;
    }
    portEXIT_CRITICAL(&mouse_lock);
    return had_click;
}

bool solar_os_mouse_take_pending_scroll(int16_t *delta, int16_t *x, int16_t *y)
{
    bool had_scroll;
    portENTER_CRITICAL(&mouse_lock);
    had_scroll = scroll_pending_delta != 0;
    if (had_scroll) {
        if (delta != NULL) *delta = scroll_pending_delta;
        if (x != NULL) *x = scroll_pending_x;
        if (y != NULL) *y = scroll_pending_y;
        scroll_pending_delta = 0;
    }
    portEXIT_CRITICAL(&mouse_lock);
    return had_scroll;
}

bool solar_os_mouse_take_pending_drag(int16_t *x, int16_t *y, int16_t *dx, int16_t *dy,
                                      uint8_t *buttons, bool *started, bool *ended)
{
    bool had_drag;
    portENTER_CRITICAL(&mouse_lock);
    had_drag = drag_pending;
    if (had_drag) {
        if (x != NULL) *x = drag_pending_x;
        if (y != NULL) *y = drag_pending_y;
        if (dx != NULL) *dx = drag_pending_dx;
        if (dy != NULL) *dy = drag_pending_dy;
        if (buttons != NULL) *buttons = drag_pending_buttons;
        if (started != NULL) *started = drag_pending_started;
        if (ended != NULL) *ended = drag_pending_ended;
        drag_pending = false;
        drag_pending_dx = 0;
        drag_pending_dy = 0;
        drag_pending_started = false;
        drag_pending_ended = false;
    }
    portEXIT_CRITICAL(&mouse_lock);
    return had_drag;
}

void solar_os_mouse_tick(uint32_t now_ms)
{
    portENTER_CRITICAL(&mouse_lock);
    if (mouse_state.visible && (now_ms - mouse_state.last_active_ms) > SOLAR_OS_MOUSE_TIMEOUT_MS) {
        mouse_state.visible = false;
        mouse_dirty = true;
    }
    portEXIT_CRITICAL(&mouse_lock);
}

/* ---------------------------------------------------------------------
 * Software Cursor Overlay Drawing (Arrow Sprite for ST7305 RLCD)
 * ------------------------------------------------------------------- */
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

/* Cursor sprite footprint in physical pixels (8 cols x 14 rows, 2x scale). */
#define MOUSE_CURSOR_PX_W 16
#define MOUSE_CURSOR_PX_H 28

static void mouse_draw_sprite_at_u8g2(u8g2_t *u8g2, int mx, int my)
{
    /* Draw white interior fill (color 0 on RLCD) */
    u8g2_SetDrawColor(u8g2, 0);
    for (int r = 0; r < 14; r++) {
        uint8_t row = cursor_fill[r];
        for (int c = 0; c < 8; c++) {
            if (row & (1 << (7 - c))) {
                const int px = mx + (c * 2);
                const int py = my + (r * 2);
                u8g2_DrawPixel(u8g2, (u8g2_uint_t)px, (u8g2_uint_t)py);
                u8g2_DrawPixel(u8g2, (u8g2_uint_t)(px + 1), (u8g2_uint_t)py);
                u8g2_DrawPixel(u8g2, (u8g2_uint_t)px, (u8g2_uint_t)(py + 1));
                u8g2_DrawPixel(u8g2, (u8g2_uint_t)(px + 1), (u8g2_uint_t)(py + 1));
            }
        }
    }

    /* Draw black crisp outline (color 1 on RLCD) */
    u8g2_SetDrawColor(u8g2, 1);
    for (int r = 0; r < 14; r++) {
        uint8_t row = cursor_mask[r];
        for (int c = 0; c < 8; c++) {
            if ((row & (1 << (7 - c))) && !(cursor_fill[r] & (1 << (7 - c)))) {
                const int px = mx + (c * 2);
                const int py = my + (r * 2);
                u8g2_DrawPixel(u8g2, (u8g2_uint_t)px, (u8g2_uint_t)py);
                u8g2_DrawPixel(u8g2, (u8g2_uint_t)(px + 1), (u8g2_uint_t)py);
                u8g2_DrawPixel(u8g2, (u8g2_uint_t)px, (u8g2_uint_t)(py + 1));
                u8g2_DrawPixel(u8g2, (u8g2_uint_t)(px + 1), (u8g2_uint_t)(py + 1));
            }
        }
    }
}

void solar_os_mouse_draw_cursor_u8g2(u8g2_t *u8g2)
{
    if (u8g2 == NULL) return;

    int mx, my;
    bool vis;
    portENTER_CRITICAL(&mouse_lock);
    mx = mouse_state.x;
    my = mouse_state.y;
    vis = mouse_state.visible;
    portEXIT_CRITICAL(&mouse_lock);

    if (!vis) return;
    mouse_draw_sprite_at_u8g2(u8g2, mx, my);
}

/* ---------------------------------------------------------------------
 * Cursor Compositor: save/restore the pixels under the sprite so the
 * cursor can be tracked between full app redraws without leaving a trail.
 * See solar_os_display_present() (the single choke point every app and
 * the terminal already push frames through) and main.c's
 * dispatch_mouse_compositor() for how these get called.
 * ------------------------------------------------------------------- */
static uint8_t cursor_backup[MOUSE_CURSOR_PX_H][MOUSE_CURSOR_PX_W];
static bool cursor_backup_valid = false;
static int cursor_backup_x = 0;
static int cursor_backup_y = 0;

void solar_os_mouse_compositor_invalidate(void)
{
    cursor_backup_valid = false;
}

void solar_os_mouse_compositor_stamp(u8g2_t *u8g2)
{
    if (u8g2 == NULL) return;

    int mx, my;
    bool vis;
    portENTER_CRITICAL(&mouse_lock);
    mx = mouse_state.x;
    my = mouse_state.y;
    vis = mouse_state.visible;
    portEXIT_CRITICAL(&mouse_lock);

    if (!vis) {
        cursor_backup_valid = false;
        return;
    }

    for (int r = 0; r < MOUSE_CURSOR_PX_H; r++) {
        for (int c = 0; c < MOUSE_CURSOR_PX_W; c++) {
            cursor_backup[r][c] = solar_os_display_get_pixel(u8g2, (uint16_t)(mx + c), (uint16_t)(my + r));
        }
    }
    cursor_backup_x = mx;
    cursor_backup_y = my;
    cursor_backup_valid = true;

    mouse_draw_sprite_at_u8g2(u8g2, mx, my);
}

static void mouse_compositor_restore(u8g2_t *u8g2)
{
    if (u8g2 == NULL || !cursor_backup_valid) return;

    for (int r = 0; r < MOUSE_CURSOR_PX_H; r++) {
        for (int c = 0; c < MOUSE_CURSOR_PX_W; c++) {
            u8g2_SetDrawColor(u8g2, cursor_backup[r][c]);
            u8g2_DrawPixel(u8g2, (u8g2_uint_t)(cursor_backup_x + c), (u8g2_uint_t)(cursor_backup_y + r));
        }
    }
    cursor_backup_valid = false;
}

void solar_os_mouse_compositor_track_tick(u8g2_t *u8g2)
{
    if (u8g2 == NULL) return;
    /* Safe as long as nothing besides our own last stamp touched this
     * region since -- true between two app redraws, since solar_os_gfx /
     * terminal always redraw their whole background before presenting,
     * which naturally overwrites any stale backup and takes the stamp
     * path (with a fresh, valid snapshot) instead of this one. */
    mouse_compositor_restore(u8g2);
    solar_os_display_present(u8g2, SOLAR_OS_DISPLAY_PRESENT_GRAPHICS);
}

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

    /* Draw white interior fill */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    for (int r = 0; r < 14; r++) {
        uint8_t row = cursor_fill[r];
        for (int c = 0; c < 8; c++) {
            if (row & (1 << (7 - c))) {
                solar_os_gfx_pixel(gfx, mx + (c * 2), my + (r * 2));
                solar_os_gfx_pixel(gfx, mx + (c * 2) + 1, my + (r * 2));
                solar_os_gfx_pixel(gfx, mx + (c * 2), my + (r * 2) + 1);
                solar_os_gfx_pixel(gfx, mx + (c * 2) + 1, my + (r * 2) + 1);
            }
        }
    }

    /* Draw black crisp outline */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    for (int r = 0; r < 14; r++) {
        uint8_t row = cursor_mask[r];
        for (int c = 0; c < 8; c++) {
            if ((row & (1 << (7 - c))) && !(cursor_fill[r] & (1 << (7 - c)))) {
                solar_os_gfx_pixel(gfx, mx + (c * 2), my + (r * 2));
                solar_os_gfx_pixel(gfx, mx + (c * 2) + 1, my + (r * 2));
                solar_os_gfx_pixel(gfx, mx + (c * 2), my + (r * 2) + 1);
                solar_os_gfx_pixel(gfx, mx + (c * 2) + 1, my + (r * 2) + 1);
            }
        }
    }
}
