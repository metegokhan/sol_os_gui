/*
 * Solar OS - BLE & System Mouse Service
 * Virtual Cursor & Navigation Engine for ST7305 RLCD (400x300)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_gfx.h"
#include "u8g2.h"

#define SOLAR_OS_MOUSE_WIDTH 400
#define SOLAR_OS_MOUSE_HEIGHT 300
#define SOLAR_OS_MOUSE_TIMEOUT_MS 5000U /* Hide cursor after 5s inactivity */

#define SOLAR_OS_MOUSE_BTN_LEFT   (1U << 0)
#define SOLAR_OS_MOUSE_BTN_RIGHT  (1U << 1)
#define SOLAR_OS_MOUSE_BTN_MIDDLE (1U << 2)

typedef struct {
    int16_t x;
    int16_t y;
    uint8_t buttons;
    int8_t wheel;
    bool visible;
    bool connected;
    uint32_t last_active_ms;
} solar_os_mouse_state_t;

esp_err_t solar_os_mouse_init(void);
void solar_os_mouse_set_connected(bool connected);
bool solar_os_mouse_is_connected(void);
/* When suppressed, incoming HID mouse reports are dropped at the source so a
 * moving mouse costs no CPU. Keyboard and gamepad input are unaffected. Used by
 * full-screen apps (e.g. the Game Boy emulator) that have no cursor. */
void solar_os_mouse_set_suppressed(bool suppressed);
bool solar_os_mouse_is_suppressed(void);
void solar_os_mouse_process_report(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel);
void solar_os_mouse_get_state(solar_os_mouse_state_t *out);
bool solar_os_mouse_is_visible(void);
bool solar_os_mouse_is_dirty(void);
void solar_os_mouse_clear_dirty(void);
/* Pops the pending left-click position (screen coords) recorded on the
 * button-press edge, if any. Returns false and leaves the out-params
 * untouched when there is no pending click. */
bool solar_os_mouse_take_pending_click(int16_t *x, int16_t *y, uint8_t *buttons);
/* Pops accumulated wheel notches since the last pop, if any. Returns false
 * and leaves the out-params untouched when there is no pending scroll. */
bool solar_os_mouse_take_pending_scroll(int16_t *delta, int16_t *x, int16_t *y);
/* Pops one pending drag step (left-button held + cursor moved, or the
 * press/release edge of a left-button hold), if any. dx/dy are relative to
 * the previous popped drag step of the same press. Returns false and
 * leaves the out-params untouched when there is no pending drag step. */
bool solar_os_mouse_take_pending_drag(int16_t *x, int16_t *y, int16_t *dx, int16_t *dy,
                                      uint8_t *buttons, bool *started, bool *ended);
void solar_os_mouse_draw_cursor(solar_os_gfx_t *gfx);
void solar_os_mouse_draw_cursor_u8g2(u8g2_t *u8g2);
void solar_os_mouse_tick(uint32_t now_ms);

/* Cursor compositor: called from solar_os_display_present() (stamp on every
 * normal frame push, invalidate when the cursor isn't visible) and from
 * main.c's independent mouse-tracking poll (track_tick, for movement that
 * happens between two app redraws). Apps should never call these directly. */
void solar_os_mouse_compositor_stamp(u8g2_t *u8g2);
void solar_os_mouse_compositor_invalidate(void);
void solar_os_mouse_compositor_track_tick(u8g2_t *u8g2);
