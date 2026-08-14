#include "solar_os_timer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"

#define TIMER_STACK_SIZE 4096
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(TIMER_STACK_SIZE);

static const uint32_t preset_seconds[] = {60, 180, 300, 600, 900, 1500};
static const char *const preset_labels[] = {"1 Min", "3 Min", "5 Min", "10 Min", "15 Min", "25 Min"};
#define PRESET_COUNT (sizeof(preset_seconds)/sizeof(preset_seconds[0]))

typedef struct {
    bool running;
    bool finished;
    int preset_idx;
    uint32_t total_seconds;
    uint32_t remaining_seconds;
    uint32_t start_tick_ms;
    uint32_t last_sec_tick;
} timer_app_state_t;

static void *timer_state_ptr;
#define tstate (*(timer_app_state_t *)timer_state_ptr)

static void draw_segment(solar_os_gfx_t *gfx, int seg, int x, int y, int w, int h, int t)
{
    const int bevel = t / 2;
    const int mid_y = y + h / 2;
    switch (seg) {
    case 0: /* Top */
        solar_os_gfx_fill_rect(gfx, x + bevel, y, w - 2 * bevel, t);
        break;
    case 1: /* Top Right */
        solar_os_gfx_fill_rect(gfx, x + w - t, y + bevel, t, h / 2 - bevel);
        break;
    case 2: /* Bottom Right */
        solar_os_gfx_fill_rect(gfx, x + w - t, mid_y, t, h / 2 - bevel);
        break;
    case 3: /* Bottom */
        solar_os_gfx_fill_rect(gfx, x + bevel, y + h - t, w - 2 * bevel, t);
        break;
    case 4: /* Bottom Left */
        solar_os_gfx_fill_rect(gfx, x, mid_y, t, h / 2 - bevel);
        break;
    case 5: /* Top Left */
        solar_os_gfx_fill_rect(gfx, x, y + bevel, t, h / 2 - bevel);
        break;
    case 6: /* Middle */
        solar_os_gfx_fill_rect(gfx, x + bevel, mid_y - t / 2, w - 2 * bevel, t);
        break;
    default:
        break;
    }
}

static void draw_7seg_digit(solar_os_gfx_t *gfx, int digit, int x, int y, int w, int h, int t)
{
    static const uint8_t masks[10] = {
        0x3F, /* 0 */
        0x06, /* 1 */
        0x5B, /* 2 */
        0x4F, /* 3 */
        0x66, /* 4 */
        0x6D, /* 5 */
        0x7D, /* 6 */
        0x07, /* 7 */
        0x7F, /* 8 */
        0x6F, /* 9 */
    };
    if (digit < 0 || digit > 9) return;
    uint8_t m = masks[digit];
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    for (int i = 0; i < 7; i++) {
        if (m & (1 << i)) {
            draw_segment(gfx, i, x, y, w, h, t);
        }
    }
}

static void timer_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 16, "COUNTDOWN TIMER");

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const char *status_str = tstate.finished ? "[ TIME'S UP! ]" : (tstate.running ? "[ COUNTING DOWN ]" : "[ PAUSED ]");
    solar_os_gfx_text(gfx, screen_w - 120, 16, status_str);

    /* 2. Giant Timer Card (X: 16..384, Y: 34..148) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 16, 34, screen_w - 32, 118);
    solar_os_gfx_rect(gfx, 18, 36, screen_w - 36, 114);

    uint32_t rem = tstate.remaining_seconds;
    uint32_t min = rem / 60U;
    uint32_t sec = rem % 60U;

    /* Render Giant 7-Segment Digits */
    const int base_x = 78;
    const int base_y = 46;
    const int dig_w = 44;
    const int dig_h = 76;
    const int dig_t = 8;

    /* Minutes */
    draw_7seg_digit(gfx, (int)(min / 10U), base_x, base_y, dig_w, dig_h, dig_t);
    draw_7seg_digit(gfx, (int)(min % 10U), base_x + 54, base_y, dig_w, dig_h, dig_t);

    /* Colon : */
    solar_os_gfx_fill_circle(gfx, base_x + 115, base_y + 24, 5);
    solar_os_gfx_fill_circle(gfx, base_x + 115, base_y + 52, 5);

    /* Seconds */
    draw_7seg_digit(gfx, (int)(sec / 10U), base_x + 138, base_y, dig_w, dig_h, dig_t);
    draw_7seg_digit(gfx, (int)(sec % 10U), base_x + 192, base_y, dig_w, dig_h, dig_t);

    /* Progress bar inside card */
    if (tstate.total_seconds > 0) {
        int bar_w = screen_w - 68;
        int fill_w = (int)((uint64_t)bar_w * (tstate.total_seconds - rem) / tstate.total_seconds);
        solar_os_gfx_rect(gfx, 34, 136, bar_w, 8);
        if (fill_w > 0) {
            solar_os_gfx_fill_rect(gfx, 36, 138, fill_w, 4);
        }
    }

    /* 3. Preset Selector Box (X: 16..384, Y: 160..268) */
    solar_os_gfx_rect(gfx, 16, 160, screen_w - 32, 110);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 28, 180, "QUICK DURATION PRESETS");
    solar_os_gfx_line(gfx, 22, 186, screen_w - 22, 186);

    const int preset_w = 54;
    const int preset_h = 30;
    const int start_x = 26;
    const int start_y = 196;
    const int gap_x = 6;

    for (size_t i = 0; i < PRESET_COUNT; i++) {
        const int px = start_x + (int)i * (preset_w + gap_x);
        const bool is_sel = (i == (size_t)tstate.preset_idx);

        if (is_sel) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, px, start_y, preset_w, preset_h);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, px, start_y, preset_w, preset_h);
        }

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        const size_t lw = solar_os_gfx_text_width(gfx, preset_labels[i]);
        solar_os_gfx_text(gfx, px + (preset_w - (int)lw) / 2, start_y + 19, preset_labels[i]);
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 28, 252, "Use [LEFT/RIGHT] to choose preset | [UP/DOWN] +/- 1 Minute");

    /* 4. Footer */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 293, "[SPACE/ENTER] Start/Pause | [ARROWS] Adjust | [R] Reset | [ESC] Exit");

    solar_os_gfx_present(gfx);
}

static esp_err_t timer_start(solar_os_context_t *ctx)
{
    tstate.running = false;
    tstate.finished = false;
    tstate.preset_idx = 2; /* 5 Min default */
    tstate.total_seconds = preset_seconds[tstate.preset_idx];
    tstate.remaining_seconds = tstate.total_seconds;
    tstate.start_tick_ms = (uint32_t)(esp_timer_get_time() / 1000U);
    tstate.last_sec_tick = tstate.start_tick_ms;

    solar_os_context_set_graphics_active(ctx, true);
    timer_render(ctx);
    return ESP_OK;
}

static void timer_stop(solar_os_context_t *ctx)
{
    tstate.running = false;
    solar_os_context_set_graphics_active(ctx, false);
}

static bool timer_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (tstate.running && !tstate.finished) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
            if (now - tstate.last_sec_tick >= 1000U) {
                tstate.last_sec_tick = now;
                if (tstate.remaining_seconds > 0) {
                    tstate.remaining_seconds--;
                    if (tstate.remaining_seconds == 0) {
                        tstate.finished = true;
                        tstate.running = false;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
                        (void)solar_os_audio_play_tone(1046, 500, 100); /* High pitch chime */
#endif
                    }
                }
                timer_render(ctx);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (ch == ' ' || ch == '\r' || ch == '\n') {
            if (tstate.finished) {
                tstate.finished = false;
                tstate.remaining_seconds = tstate.total_seconds;
                tstate.running = true;
                tstate.last_sec_tick = (uint32_t)(esp_timer_get_time() / 1000U);
            } else {
                tstate.running = !tstate.running;
                if (tstate.running) {
                    tstate.last_sec_tick = (uint32_t)(esp_timer_get_time() / 1000U);
                }
            }
            timer_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A' || ch == 'h' || ch == 'H') {
            if (!tstate.running) {
                if (tstate.preset_idx > 0) {
                    tstate.preset_idx--;
                } else {
                    tstate.preset_idx = (int)PRESET_COUNT - 1;
                }
                tstate.total_seconds = preset_seconds[tstate.preset_idx];
                tstate.remaining_seconds = tstate.total_seconds;
                tstate.finished = false;
                timer_render(ctx);
            }
            return true;
        }

        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D' || ch == 'l' || ch == 'L') {
            if (!tstate.running) {
                if (tstate.preset_idx + 1 < (int)PRESET_COUNT) {
                    tstate.preset_idx++;
                } else {
                    tstate.preset_idx = 0;
                }
                tstate.total_seconds = preset_seconds[tstate.preset_idx];
                tstate.remaining_seconds = tstate.total_seconds;
                tstate.finished = false;
                timer_render(ctx);
            }
            return true;
        }

        if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W' || ch == 'k' || ch == 'K') {
            if (!tstate.running) {
                tstate.total_seconds += 60;
                if (tstate.total_seconds > 3600) tstate.total_seconds = 3600;
                tstate.remaining_seconds = tstate.total_seconds;
                tstate.finished = false;
                timer_render(ctx);
            }
            return true;
        }

        if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S' || ch == 'j' || ch == 'J') {
            if (!tstate.running && tstate.total_seconds > 60) {
                tstate.total_seconds -= 60;
                tstate.remaining_seconds = tstate.total_seconds;
                tstate.finished = false;
                timer_render(ctx);
            }
            return true;
        }

        if (ch == 'r' || ch == 'R') {
            tstate.running = false;
            tstate.finished = false;
            tstate.total_seconds = preset_seconds[tstate.preset_idx];
            tstate.remaining_seconds = tstate.total_seconds;
            timer_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

static uint32_t timer_tick_ms(void)
{
    return 200U;
}

const solar_os_app_t solar_os_timer_app = {
    .name = "timer",
    .summary = "countdown timer with audible alarm",
    .flags = 0,
    .start = timer_start,
    .stop = timer_stop,
    .event = timer_event,
    .state_slot = &timer_state_ptr,
    .state_size = sizeof(timer_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = TIMER_STACK_SIZE,
    .requested_tick_interval_ms = timer_tick_ms,
};
