#include "solar_os_pomodoro.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"
#include "solar_os_appbar.h"

#define POMODORO_STACK_SIZE 4096
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(POMODORO_STACK_SIZE);

typedef enum {
    POMO_MODE_WORK = 0,
    POMO_MODE_SHORT_BREAK = 1,
    POMO_MODE_LONG_BREAK = 2,
} pomo_mode_t;

typedef struct {
    pomo_mode_t mode;
    bool running;
    uint32_t total_seconds;
    uint32_t remaining_seconds;
    uint32_t completed_cycles;
    uint32_t last_tick_ms;
} pomo_state_t;

static void *pomo_state_ptr;
#define pomo (*(pomo_state_t *)pomo_state_ptr)

static const uint32_t pomo_durations[] = {
    25 * 60, /* 25 min work */
    5 * 60,  /* 5 min short break */
    15 * 60, /* 15 min long break */
};

static const char *const pomo_mode_names[] = {
    "FOCUS TIME",
    "SHORT BREAK",
    "LONG BREAK"
};

/* Compact labels for the shared header tab strip. */
static const char *const pomo_tab_names[] = { "Focus", "Short", "Long" };

/* Footer chips: Start/Pause + Reset. Same set feeds draw and click. */
static size_t pomo_build_footer(bool running, solar_os_appbar_shortcut_t *items, size_t max_items)
{
    size_t n = 0;
    if (n < max_items) { items[n].key = ' '; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), running ? "Pause" : "Start"); n++; }
    if (n < max_items) { items[n].key = 'r'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Reset"); n++; }
    return n;
}

static void pomo_set_mode(pomo_mode_t mode)
{
    pomo.mode = mode;
    pomo.total_seconds = pomo_durations[mode];
    pomo.remaining_seconds = pomo.total_seconds;
    pomo.running = false;
}

static void pomo_render(solar_os_context_t *ctx)
{
    if (!solar_os_context_graphics_active(ctx)) return;
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Shared header with the three modes as tabs. */
    solar_os_appbar_header_t header = {0};
    header.title = "Pomodoro";
    header.show_back = true;
    header.tabs.names = pomo_tab_names;
    header.tabs.count = 3;
    header.tabs.active_index = (size_t)pomo.mode;
    char status_line[48];
    snprintf(status_line, sizeof(status_line), "%s   Completed: %u",
             pomo_mode_names[pomo.mode], (unsigned)pomo.completed_cycles);
    header.status_line = status_line;
    solar_os_appbar_draw_header(gfx, &header);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    /* 3. Center Circular Progress Dial (Center: 200, 150, Radius: 75) */
    const int cx = 200;
    const int cy = 150;
    const int radius = 75;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_circle(gfx, cx, cy, radius);
    solar_os_gfx_circle(gfx, cx, cy, radius - 1);
    solar_os_gfx_circle(gfx, cx, cy, radius - 8);

    /* Filled arc / segmented progress ring */
    const float fraction = (pomo.total_seconds > 0) ?
        ((float)(pomo.total_seconds - pomo.remaining_seconds) / (float)pomo.total_seconds) : 0.0f;
    const int total_ticks = 48;
    const int filled_ticks = (int)(fraction * (float)total_ticks);

    for (int t = 0; t < total_ticks; t++) {
        const float angle = ((float)t / (float)total_ticks) * 2.0f * (float)M_PI - ((float)M_PI / 2.0f);
        const int x0 = cx + (int)((float)(radius - 7) * cosf(angle));
        const int y0 = cy + (int)((float)(radius - 7) * sinf(angle));
        const int x1 = cx + (int)((float)(radius - 1) * cosf(angle));
        const int y1 = cy + (int)((float)(radius - 1) * sinf(angle));

        if (t <= filled_ticks) {
            solar_os_gfx_line(gfx, x0, y0, x1, y1);
        }
    }

    /* Large Countdown Digits inside circle */
    const uint32_t m = pomo.remaining_seconds / 60;
    const uint32_t s = pomo.remaining_seconds % 60;
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02u:%02u", (unsigned)m, (unsigned)s);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    const size_t digits_w = solar_os_gfx_text_width(gfx, time_str);
    solar_os_gfx_text(gfx, cx - (int)digits_w / 2, cy + 6, time_str);

    /* State text inside circle below time */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const char *status_str = pomo.running ? "[ RUNNING ]" : "[ PAUSED ]";
    const size_t sw = solar_os_gfx_text_width(gfx, status_str);
    solar_os_gfx_text(gfx, cx - (int)sw / 2, cy + 24, status_str);

    /* 4. Bottom Cycle Progress Indicators */
    char cycle_bar[64] = "CYCLES: ";
    for (uint32_t i = 0; i < 4; i++) {
        if (i < pomo.completed_cycles % 4) {
            strlcat(cycle_bar, "[*] ", sizeof(cycle_bar));
        } else {
            strlcat(cycle_bar, "[ ] ", sizeof(cycle_bar));
        }
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    const size_t cb_w = solar_os_gfx_text_width(gfx, cycle_bar);
    solar_os_gfx_text(gfx, (screen_w - (int)cb_w) / 2, 260, cycle_bar);

    /* 5. Shared footer chips. */
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = pomo_build_footer(pomo.running, items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);

    solar_os_gfx_present(gfx);
}

static esp_err_t pomo_start(solar_os_context_t *ctx)
{
    pomo.completed_cycles = 0;
    pomo.last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000U);
    pomo_set_mode(POMO_MODE_WORK);

    solar_os_context_set_graphics_active(ctx, true);
    pomo_render(ctx);
    return ESP_OK;
}

static void pomo_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void pomo_suspend(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void pomo_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    if (pomo.running && pomo.last_tick_ms > 0) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
        const uint32_t elapsed_sec = (now > pomo.last_tick_ms) ? (now - pomo.last_tick_ms) / 1000U : 0;
        if (elapsed_sec > 0) {
            pomo.last_tick_ms += elapsed_sec * 1000U;
            if (elapsed_sec >= pomo.remaining_seconds) {
                pomo.remaining_seconds = 0;
                pomo.running = false;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
                (void)solar_os_audio_play_tone(1000, 300, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
#endif
                if (pomo.mode == POMO_MODE_WORK) {
                    pomo.completed_cycles++;
                    if (pomo.completed_cycles % 4 == 0) {
                        pomo_set_mode(POMO_MODE_LONG_BREAK);
                    } else {
                        pomo_set_mode(POMO_MODE_SHORT_BREAK);
                    }
                } else {
                    pomo_set_mode(POMO_MODE_WORK);
                }
            } else {
                pomo.remaining_seconds -= elapsed_sec;
            }
        }
    }
    pomo_render(ctx);
}

static bool pomo_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        pomo_resume(ctx);
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
        if (pomo.running && (now - pomo.last_tick_ms >= 1000U)) {
            pomo.last_tick_ms = now;
            if (pomo.remaining_seconds > 0) {
                pomo.remaining_seconds--;
            }
            if (pomo.remaining_seconds == 0) {
                pomo.running = false;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
                (void)solar_os_audio_play_tone(1000, 300, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
#endif
                if (pomo.mode == POMO_MODE_WORK) {
                    pomo.completed_cycles++;
                    if (pomo.completed_cycles % 4 == 0) {
                        pomo_set_mode(POMO_MODE_LONG_BREAK);
                    } else {
                        pomo_set_mode(POMO_MODE_SHORT_BREAK);
                    }
                } else {
                    pomo_set_mode(POMO_MODE_WORK);
                }
            }
            pomo_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        solar_os_appbar_header_t header = {0};
        header.show_back = true;
        header.tabs.names = pomo_tab_names;
        header.tabs.count = 3;
        header.tabs.active_index = (size_t)pomo.mode;
        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, px, py, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                solar_os_context_request_exit(ctx);
            } else if (hit.kind == SOLAR_OS_APPBAR_HIT_TAB_ITEM && hit.index < 3) {
                pomo_set_mode((pomo_mode_t)hit.index);
                pomo_render(ctx);
            }
            return true;
        }

        solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
        const size_t count = pomo_build_footer(pomo.running, items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < count) {
                if (items[fhit.index].key == 'r') {
                    pomo_set_mode(pomo.mode);
                } else {
                    pomo.running = !pomo.running;
                    pomo.last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000U);
                }
                pomo_render(ctx);
            }
            return true;
        }

        /* Tap the dial to start/pause. */
        const int dx = px - 200;
        const int dy = py - 150;
        if (dx * dx + dy * dy <= 75 * 75) {
            pomo.running = !pomo.running;
            pomo.last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000U);
            pomo_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;
        if (ch == ' ') {
            pomo.running = !pomo.running;
            pomo.last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000U);
            pomo_render(ctx);
            return true;
        }
        if (ch == 'r' || ch == 'R') {
            pomo_set_mode(pomo.mode);
            pomo_render(ctx);
            return true;
        }
        if (ch == '\t') {
            pomo_mode_t next = (pomo.mode + 1) % 3;
            pomo_set_mode(next);
            pomo_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_pomodoro_app = {
    .name = "pomodoro",
    .summary = "visual circular pomodoro focus timer",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = pomo_start,
    .suspend = pomo_suspend,
    .resume = pomo_resume,
    .stop = pomo_stop,
    .event = pomo_event,
    .state_slot = &pomo_state_ptr,
    .state_size = sizeof(pomo_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 250U,
    .worker_stack_bytes = POMODORO_STACK_SIZE,
};
