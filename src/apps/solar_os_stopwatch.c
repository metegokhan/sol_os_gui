#include "solar_os_stopwatch.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"
#include "solar_os_appbar.h"
#include "solar_os_help.h"

#define STOPWATCH_STACK_SIZE 4096
#define MAX_LAPS 8
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(STOPWATCH_STACK_SIZE);

typedef struct {
    bool running;
    uint32_t start_time_ms;
    uint32_t elapsed_ms;
    uint32_t laps[MAX_LAPS];
    size_t lap_count;
    uint32_t last_tick_ms;
    bool show_help;
} stopwatch_state_t;

static void *stopwatch_state_ptr;
#define swstate (*(stopwatch_state_t *)stopwatch_state_ptr)

static const char *const sw_help_lines[] = {
    "A precision stopwatch with lap recording.",
    "",
    "Controls:",
    "  Start / Stop : tap the timer, the Start/Stop chip,",
    "                 or press Space / Enter.",
    "  Lap          : Lap chip or press L (while running).",
    "  Reset        : Reset chip or press R.",
    "  Exit         : Back arrow or Esc / Q.",
    "",
    "Laps show as MM:SS.cc, up to 8 recorded.",
};
#define SW_HELP_LINE_COUNT (sizeof(sw_help_lines) / sizeof(sw_help_lines[0]))

static void sw_toggle(void)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
    if (swstate.running) {
        swstate.elapsed_ms += (now - swstate.start_time_ms);
        swstate.running = false;
    } else {
        swstate.start_time_ms = now;
        swstate.running = true;
    }
}

static void sw_lap(void)
{
    if (swstate.running && swstate.lap_count < MAX_LAPS) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
        swstate.laps[swstate.lap_count++] = swstate.elapsed_ms + (now - swstate.start_time_ms);
    }
}

static void sw_reset(void)
{
    swstate.running = false;
    swstate.elapsed_ms = 0;
    swstate.start_time_ms = 0;
    swstate.lap_count = 0;
}

/* Footer chips: Start/Stop, Lap, Reset, Help. Same set feeds draw and click. */
static size_t sw_build_footer(solar_os_appbar_shortcut_t *items, size_t max_items)
{
    size_t n = 0;
    if (n < max_items) { items[n].key = ' '; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), swstate.running ? "Stop" : "Start"); n++; }
    if (n < max_items) { items[n].key = 'l'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Lap"); n++; }
    if (n < max_items) { items[n].key = 'r'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Reset"); n++; }
    if (n < max_items) { solar_os_help_chip(&items[n]); n++; }
    return n;
}

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

static void stopwatch_render(solar_os_context_t *ctx)
{
    if (!solar_os_context_graphics_active(ctx)) return;
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Shared header. */
    solar_os_appbar_header_t header = {0};
    header.title = "Stopwatch";
    header.show_back = true;
    header.status_line = swstate.running ? "Running" : "Stopped";
    solar_os_appbar_draw_header(gfx, &header);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    /* 2. Giant Chrono Display Card (X: 16..384, Y: 34..134) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 16, 34, screen_w - 32, 104);
    solar_os_gfx_rect(gfx, 18, 36, screen_w - 36, 100);

    uint32_t total = swstate.elapsed_ms;
    if (swstate.running) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
        total += (now - swstate.start_time_ms);
    }

    uint32_t total_sec = total / 1000U;
    uint32_t min = total_sec / 60U;
    uint32_t sec = total_sec % 60U;
    uint32_t cs = (total % 1000U) / 10U; /* Centiseconds */

    /* Render Giant 7-Segment Digits */
    const int base_x = 56;
    const int base_y = 52;
    const int dig_w = 34;
    const int dig_h = 66;
    const int dig_t = 7;

    /* Minutes */
    draw_7seg_digit(gfx, (int)(min / 10U), base_x, base_y, dig_w, dig_h, dig_t);
    draw_7seg_digit(gfx, (int)(min % 10U), base_x + 42, base_y, dig_w, dig_h, dig_t);

    /* Colon : */
    solar_os_gfx_fill_circle(gfx, base_x + 88, base_y + 20, 4);
    solar_os_gfx_fill_circle(gfx, base_x + 88, base_y + 46, 4);

    /* Seconds */
    draw_7seg_digit(gfx, (int)(sec / 10U), base_x + 102, base_y, dig_w, dig_h, dig_t);
    draw_7seg_digit(gfx, (int)(sec % 10U), base_x + 144, base_y, dig_w, dig_h, dig_t);

    /* Dot . */
    solar_os_gfx_fill_circle(gfx, base_x + 188, base_y + 60, 4);

    /* Centiseconds (slightly smaller offset for clean visual balance) */
    draw_7seg_digit(gfx, (int)(cs / 10U), base_x + 200, base_y + 14, 26, 52, 5);
    draw_7seg_digit(gfx, (int)(cs % 10U), base_x + 234, base_y + 14, 26, 52, 5);

    /* Unit labels */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, base_x + 24, base_y + 78, "MIN");
    solar_os_gfx_text(gfx, base_x + 126, base_y + 78, "SEC");
    solar_os_gfx_text(gfx, base_x + 224, base_y + 78, "1/100");

    /* 3. Laps / Splits Box (X: 16..384, Y: 146..268) */
    solar_os_gfx_rect(gfx, 16, 146, screen_w - 32, 124);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 28, 165, "RECORDED LAP TIMES");
    solar_os_gfx_line(gfx, 22, 171, screen_w - 22, 171);

    if (swstate.lap_count == 0) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 28, 195, "No laps yet. Tap Lap while running to record one.");
    } else {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        for (size_t i = 0; i < swstate.lap_count && i < MAX_LAPS; i++) {
            uint32_t l_total = swstate.laps[i];
            uint32_t l_sec = l_total / 1000U;
            uint32_t l_min = l_sec / 60U;
            uint32_t l_s = l_sec % 60U;
            uint32_t l_cs = (l_total % 1000U) / 10U;

            char lap_str[48];
            snprintf(lap_str, sizeof(lap_str), "Lap %u:   %02u:%02u.%02u",
                     (unsigned)(i + 1), (unsigned)l_min, (unsigned)l_s, (unsigned)l_cs);

            const int col = (int)(i / 4);
            const int row = (int)(i % 4);
            solar_os_gfx_text(gfx, 28 + col * 170, 192 + row * 18, lap_str);
        }
    }

    /* 4. Shared footer chips. */
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = sw_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);

    /* Help overlay on top. */
    if (swstate.show_help) {
        solar_os_help_draw(gfx, "Stopwatch - Help", sw_help_lines, SW_HELP_LINE_COUNT);
    }

    solar_os_gfx_present(gfx);
}

static esp_err_t stopwatch_start(solar_os_context_t *ctx)
{
    swstate.running = false;
    swstate.start_time_ms = 0;
    swstate.elapsed_ms = 0;
    swstate.lap_count = 0;
    swstate.last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000U);

    solar_os_context_set_graphics_active(ctx, true);
    stopwatch_render(ctx);
    return ESP_OK;
}

static void stopwatch_stop(solar_os_context_t *ctx)
{
    swstate.running = false;
    solar_os_context_set_graphics_active(ctx, false);
}

static void stopwatch_suspend(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void stopwatch_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    stopwatch_render(ctx);
}

static bool stopwatch_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        stopwatch_resume(ctx);
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (swstate.running) {
            stopwatch_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        if (swstate.show_help) {
            swstate.show_help = false;
            stopwatch_render(ctx);
            return true;
        }

        solar_os_appbar_header_t header = {0};
        header.show_back = true;
        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, px, py, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                solar_os_context_request_exit(ctx);
            }
            return true;
        }

        solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
        const size_t count = sw_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < count) {
                switch (items[fhit.index].key) {
                case ' ': sw_toggle(); break;
                case 'l': sw_lap(); break;
                case 'r': sw_reset(); break;
                case 'H': swstate.show_help = true; break;
                default: break;
                }
                stopwatch_render(ctx);
            }
            return true;
        }

        /* Tap the chrono card to start/stop. */
        const int screen_w = (int)solar_os_gfx_width(gfx);
        if (px >= 16 && px < screen_w - 16 && py >= 34 && py < 138) {
            sw_toggle();
            stopwatch_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (swstate.show_help) {
            swstate.show_help = false;
            stopwatch_render(ctx);
            return true;
        }
        if (solar_os_help_char_opens(ch)) {
            swstate.show_help = true;
            stopwatch_render(ctx);
            return true;
        }

        if (ch == ' ' || ch == '\r' || ch == '\n') {
            sw_toggle();
            stopwatch_render(ctx);
            return true;
        }

        if (ch == 'l' || ch == 'L' || ch == '\t') {
            sw_lap();
            stopwatch_render(ctx);
            return true;
        }

        if (ch == 'r' || ch == 'R') {
            sw_reset();
            stopwatch_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

static uint32_t stopwatch_tick_ms(void)
{
    return 50U; /* 20 FPS refresh while running */
}

const solar_os_app_t solar_os_stopwatch_app = {
    .name = "stopwatch",
    .summary = "precision stopwatch with lap recorder",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = stopwatch_start,
    .suspend = stopwatch_suspend,
    .resume = stopwatch_resume,
    .stop = stopwatch_stop,
    .event = stopwatch_event,
    .state_slot = &stopwatch_state_ptr,
    .state_size = sizeof(stopwatch_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = STOPWATCH_STACK_SIZE,
    .requested_tick_interval_ms = stopwatch_tick_ms,
};
