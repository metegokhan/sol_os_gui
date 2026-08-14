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

static void pomo_set_mode(pomo_mode_t mode)
{
    pomo.mode = mode;
    pomo.total_seconds = pomo_durations[mode];
    pomo.remaining_seconds = pomo.total_seconds;
    pomo.running = false;
}

static void pomo_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header Bar */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 16, "POMODORO TIMER");

    char cycle_str[32];
    snprintf(cycle_str, sizeof(cycle_str), "COMPLETED: %u", (unsigned)pomo.completed_cycles);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, screen_w - 110, 16, cycle_str);

    /* 2. Mode Tabs (Y: 28..44) */
    const int tab_w = (screen_w - 20) / 3;
    for (int i = 0; i < 3; i++) {
        const int tx = 10 + i * tab_w;
        const bool is_active = (i == (int)pomo.mode);

        if (is_active) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, tx, 28, tab_w - 4, 16);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, tx, 28, tab_w - 4, 16);
        }

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        const size_t tw = solar_os_gfx_text_width(gfx, pomo_mode_names[i]);
        solar_os_gfx_text(gfx, tx + (tab_w - (int)tw) / 2, 40, pomo_mode_names[i]);
    }

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
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    const size_t cb_w = solar_os_gfx_text_width(gfx, cycle_bar);
    solar_os_gfx_text(gfx, (screen_w - (int)cb_w) / 2, 260, cycle_bar);

    /* 5. Footer Navigation */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 293, "[SPACE] Start/Pause | [R] Reset | [TAB] Mode | [ESC] Exit");

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

static bool pomo_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

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
    .flags = 0,
    .start = pomo_start,
    .stop = pomo_stop,
    .event = pomo_event,
    .state_slot = &pomo_state_ptr,
    .state_size = sizeof(pomo_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = POMODORO_STACK_SIZE,
};
