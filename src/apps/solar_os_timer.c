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
#include "solar_os_appbar.h"
#include "solar_os_help.h"

#define TIMER_STACK_SIZE 4096
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(TIMER_STACK_SIZE);

/* Two rows of presets: the original short ones, then longer durations. */
static const uint32_t preset_seconds[] = {
    60, 180, 300, 600, 900, 1500,      /* row 1: 1..25 min */
    1800, 2700, 3600, 7200, 10800,     /* row 2: 30..180 min */
};
static const char *const preset_labels[] = {
    "1m", "3m", "5m", "10m", "15m", "25m",
    "30m", "45m", "60m", "120m", "180m",
};
#define PRESET_COUNT (sizeof(preset_seconds)/sizeof(preset_seconds[0]))
#define PRESET_ROW1 6                  /* first row holds this many */
#define TIMER_MAX_SECONDS 21600U       /* 6 hours */

/* How long the end-of-timer alarm keeps beeping. */
static const uint32_t alarm_secs[] = {3, 5, 10, 30};
static const char *const alarm_labels[] = {"3s", "5s", "10s", "30s"};
#define ALARM_COUNT (sizeof(alarm_secs)/sizeof(alarm_secs[0]))
#define ALARM_BEEP_INTERVAL_MS 700U

typedef struct {
    bool running;
    bool finished;
    int preset_idx;
    uint32_t total_seconds;
    uint32_t remaining_seconds;
    uint32_t start_tick_ms;
    uint32_t last_sec_tick;

    int alarm_idx;             /* into alarm_secs */
    bool alarm_active;         /* currently sounding */
    uint32_t alarm_start_ms;
    uint32_t last_beep_ms;
    bool blink;                /* TIME'S UP blink phase */
    bool show_help;
} timer_app_state_t;

static void *timer_state_ptr;
#define tstate (*(timer_app_state_t *)timer_state_ptr)

static const char *const timer_help_lines[] = {
    "A countdown timer with an audible alarm.",
    "",
    "  - Tap a preset (1m..180m, two rows), or Left/Right.",
    "  - Up/Down or scroll add/remove a minute (stopped).",
    "  - Tap the timer or Start/Pause to run it.",
    "  - When it finishes, the alarm beeps for the set",
    "    duration; Alarm cycles 3 / 5 / 10 / 30 seconds.",
    "  - Any key or tap stops the alarm early.",
    "",
    "Reset returns to the preset; Esc / Back exits.",
};
#define TIMER_HELP_LINE_COUNT (sizeof(timer_help_lines) / sizeof(timer_help_lines[0]))

static void timer_beep(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    (void)solar_os_audio_play_tone(1046, 180, 100);
    (void)solar_os_audio_play_tone(784, 180, 100);
#endif
}

static void timer_toggle(void)
{
    tstate.alarm_active = false;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
    if (tstate.finished) {
        tstate.finished = false;
        tstate.remaining_seconds = tstate.total_seconds;
        tstate.running = true;
        tstate.last_sec_tick = now;
    } else {
        tstate.running = !tstate.running;
        if (tstate.running) tstate.last_sec_tick = now;
    }
}

static void timer_reset(void)
{
    tstate.alarm_active = false;
    tstate.running = false;
    tstate.finished = false;
    tstate.total_seconds = preset_seconds[tstate.preset_idx];
    tstate.remaining_seconds = tstate.total_seconds;
}

static void timer_set_preset(int idx)
{
    if (tstate.running) return;
    tstate.preset_idx = idx;
    tstate.total_seconds = preset_seconds[idx];
    tstate.remaining_seconds = tstate.total_seconds;
    tstate.finished = false;
    tstate.alarm_active = false;
}

/* Nudges the timer duration by `delta` seconds (while stopped), clamped. */
static void timer_adjust_seconds(int delta)
{
    if (tstate.running) return;
    long v = (long)tstate.total_seconds + delta;
    if (v < 60) v = 60;
    if (v > (long)TIMER_MAX_SECONDS) v = (long)TIMER_MAX_SECONDS;
    tstate.total_seconds = (uint32_t)v;
    tstate.remaining_seconds = tstate.total_seconds;
    tstate.finished = false;
    tstate.alarm_active = false;
}

/* Preset button rectangle (two rows), shared by draw and click. */
static void timer_preset_rect(int i, int *px, int *py, int *pw, int *ph)
{
    static const int row_y[2] = {196, 231};
    const int preset_w = 54, preset_h = 30, start_x = 26, gap_x = 6;
    const int row = i < PRESET_ROW1 ? 0 : 1;
    const int col = i < PRESET_ROW1 ? i : i - PRESET_ROW1;
    *px = start_x + col * (preset_w + gap_x);
    *py = row_y[row];
    *pw = preset_w;
    *ph = preset_h;
}

static size_t timer_build_footer(solar_os_appbar_shortcut_t *items, size_t max)
{
    size_t n = 0;
    if (n < max) { items[n].key = ' '; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "%s",
                 tstate.finished ? "Restart" : (tstate.running ? "Pause" : "Start")); n++; }
    if (n < max) { items[n].key = 'r'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Reset"); n++; }
    if (n < max) { items[n].key = 'm'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Alarm:%s", alarm_labels[tstate.alarm_idx]); n++; }
    if (n < max) { solar_os_help_chip(&items[n]); n++; }
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

static void timer_render(solar_os_context_t *ctx)
{
    if (!solar_os_context_graphics_active(ctx)) return;
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Shared header. */
    solar_os_appbar_header_t header = {0};
    header.title = "Timer";
    header.show_back = true;
    char status_line[64];
    const char *state_str = tstate.finished ? "TIME'S UP!" : (tstate.running ? "Counting down" : "Ready");
    snprintf(status_line, sizeof(status_line), "%s   Alarm: %s", state_str, alarm_labels[tstate.alarm_idx]);
    header.status_line = status_line;
    solar_os_appbar_draw_header(gfx, &header);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    /* 2. Giant Timer Card (X: 16..384, Y: 34..148) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 16, 34, screen_w - 32, 118);
    solar_os_gfx_rect(gfx, 18, 36, screen_w - 36, 114);

    uint32_t rem = tstate.remaining_seconds;
    /* Always MMM:SS -- minutes get three digits (up to 360) since the wide
     * card has room; the hundreds digit is blanked below 100 minutes. */
    const uint32_t mins = rem / 60U;
    const uint32_t sec = rem % 60U;

    /* Render Giant 7-Segment Digits: m_h m_t m_o : s_t s_o */
    const int dig_w = 40;
    const int dig_h = 76;
    const int dig_t = 8;
    const int pitch = 48;
    const int total_w = 4 * pitch + 18 + dig_w; /* 5 digits + colon gap */
    const int base_x = (screen_w - total_w) / 2;
    const int base_y = 46;

    int x = base_x;
    if (mins >= 100U) {
        draw_7seg_digit(gfx, (int)((mins / 100U) % 10U), x, base_y, dig_w, dig_h, dig_t);
    }
    x += pitch;
    draw_7seg_digit(gfx, (int)((mins / 10U) % 10U), x, base_y, dig_w, dig_h, dig_t);
    x += pitch;
    draw_7seg_digit(gfx, (int)(mins % 10U), x, base_y, dig_w, dig_h, dig_t);
    x += pitch;

    /* Colon */
    solar_os_gfx_fill_circle(gfx, x + 9, base_y + 24, 5);
    solar_os_gfx_fill_circle(gfx, x + 9, base_y + 52, 5);
    x += 18;

    draw_7seg_digit(gfx, (int)(sec / 10U), x, base_y, dig_w, dig_h, dig_t);
    x += pitch;
    draw_7seg_digit(gfx, (int)(sec % 10U), x, base_y, dig_w, dig_h, dig_t);

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

    for (size_t i = 0; i < PRESET_COUNT; i++) {
        int px, py, pw, ph;
        timer_preset_rect((int)i, &px, &py, &pw, &ph);
        const bool is_sel = (i == (size_t)tstate.preset_idx);

        if (is_sel) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, px, py, pw, ph);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, px, py, pw, ph);
        }

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        const size_t lw = solar_os_gfx_text_width(gfx, preset_labels[i]);
        solar_os_gfx_text(gfx, px + (pw - (int)lw) / 2, py + 19, preset_labels[i]);
    }

    /* Blink a heavy frame around the timer while the alarm is sounding. */
    if (tstate.alarm_active && tstate.blink) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, 20, 38, screen_w - 40, 110);
        solar_os_gfx_rect(gfx, 22, 40, screen_w - 44, 106);
    }

    /* 4. Shared footer chips. */
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = timer_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);

    if (tstate.show_help) {
        solar_os_help_draw(gfx, "Timer - Help", timer_help_lines, TIMER_HELP_LINE_COUNT);
    }

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
    tstate.alarm_idx = 1; /* 5 s default */
    tstate.alarm_active = false;
    tstate.blink = false;
    tstate.show_help = false;

    solar_os_context_set_graphics_active(ctx, true);
    timer_render(ctx);
    return ESP_OK;
}

static void timer_stop(solar_os_context_t *ctx)
{
    tstate.running = false;
    solar_os_context_set_graphics_active(ctx, false);
}

static void timer_suspend(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void timer_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    if (tstate.running && !tstate.finished && tstate.last_sec_tick > 0) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
        const uint32_t elapsed_sec = (now > tstate.last_sec_tick) ? (now - tstate.last_sec_tick) / 1000U : 0;
        if (elapsed_sec > 0) {
            tstate.last_sec_tick += elapsed_sec * 1000U;
            if (elapsed_sec >= tstate.remaining_seconds) {
                tstate.remaining_seconds = 0;
                tstate.finished = true;
                tstate.running = false;
                tstate.alarm_active = true;
                tstate.alarm_start_ms = now;
                tstate.last_beep_ms = now;
                tstate.blink = true;
                timer_beep();
            } else {
                tstate.remaining_seconds -= elapsed_sec;
            }
        }
    }
    timer_render(ctx);
}

static bool timer_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        timer_resume(ctx);
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);

        if (tstate.running && !tstate.finished) {
            if (now - tstate.last_sec_tick >= 1000U) {
                tstate.last_sec_tick = now;
                if (tstate.remaining_seconds > 0) {
                    tstate.remaining_seconds--;
                    if (tstate.remaining_seconds == 0) {
                        tstate.finished = true;
                        tstate.running = false;
                        tstate.alarm_active = true;
                        tstate.alarm_start_ms = now;
                        tstate.last_beep_ms = now;
                        tstate.blink = true;
                        timer_beep();
                    }
                }
                timer_render(ctx);
            }
        }

        /* Keep the alarm beeping and blinking for the configured duration. */
        if (tstate.alarm_active) {
            if (now - tstate.alarm_start_ms >= alarm_secs[tstate.alarm_idx] * 1000U) {
                tstate.alarm_active = false;
                tstate.blink = false;
                timer_render(ctx);
            } else {
                bool dirty = false;
                if (now - tstate.last_beep_ms >= ALARM_BEEP_INTERVAL_MS) {
                    tstate.last_beep_ms = now;
                    tstate.blink = !tstate.blink;
                    timer_beep();
                    dirty = true;
                }
                if (dirty) timer_render(ctx);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int screen_w = (int)solar_os_gfx_width(gfx);
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        if (tstate.show_help) {
            tstate.show_help = false;
            timer_render(ctx);
            return true;
        }
        /* A tap during the alarm just silences it. */
        if (tstate.alarm_active) {
            tstate.alarm_active = false;
            tstate.blink = false;
            timer_render(ctx);
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
        const size_t count = timer_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < count) {
                switch (items[fhit.index].key) {
                case ' ': timer_toggle(); break;
                case 'r': timer_reset(); break;
                case 'm': tstate.alarm_idx = (tstate.alarm_idx + 1) % (int)ALARM_COUNT; break;
                case 'H': tstate.show_help = true; break;
                default: break;
                }
                timer_render(ctx);
            }
            return true;
        }

        /* Tap the timer card to start/pause. */
        if (px >= 16 && px < screen_w - 16 && py >= 34 && py < 152) {
            timer_toggle();
            timer_render(ctx);
            return true;
        }

        /* Tap a preset box (either row). */
        for (size_t i = 0; i < PRESET_COUNT; i++) {
            int bx, by, bw, bh;
            timer_preset_rect((int)i, &bx, &by, &bw, &bh);
            if (px >= bx && px < bx + bw && py >= by && py < by + bh) {
                timer_set_preset((int)i);
                timer_render(ctx);
                break;
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_SCROLL) {
        /* Scroll the wheel over the timer to fine-tune minutes. */
        if (!tstate.running && !tstate.alarm_active && !tstate.show_help) {
            timer_adjust_seconds(event->data.scroll.delta > 0 ? +60 : -60);
            timer_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (tstate.show_help) {
            tstate.show_help = false;
            timer_render(ctx);
            return true;
        }
        if (solar_os_help_char_opens(ch)) {
            tstate.show_help = true;
            timer_render(ctx);
            return true;
        }
        if (tstate.alarm_active) {
            tstate.alarm_active = false;
            tstate.blink = false;
            timer_render(ctx);
            return true;
        }

        if (ch == ' ' || ch == '\r' || ch == '\n') {
            timer_toggle();
            timer_render(ctx);
            return true;
        }

        if (ch == 'm' || ch == 'M') {
            tstate.alarm_idx = (tstate.alarm_idx + 1) % (int)ALARM_COUNT;
            timer_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A' || ch == 'h') {
            if (!tstate.running) {
                timer_set_preset(tstate.preset_idx > 0 ? tstate.preset_idx - 1 : (int)PRESET_COUNT - 1);
                timer_render(ctx);
            }
            return true;
        }

        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D' || ch == 'l' || ch == 'L') {
            if (!tstate.running) {
                timer_set_preset(tstate.preset_idx + 1 < (int)PRESET_COUNT ? tstate.preset_idx + 1 : 0);
                timer_render(ctx);
            }
            return true;
        }

        if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W' || ch == 'k' || ch == 'K') {
            timer_adjust_seconds(+60);
            timer_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S' || ch == 'j' || ch == 'J') {
            timer_adjust_seconds(-60);
            timer_render(ctx);
            return true;
        }

        if (ch == 'r' || ch == 'R') {
            timer_reset();
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
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = timer_start,
    .suspend = timer_suspend,
    .resume = timer_resume,
    .stop = timer_stop,
    .event = timer_event,
    .state_slot = &timer_state_ptr,
    .state_size = sizeof(timer_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = TIMER_STACK_SIZE,
    .requested_tick_interval_ms = timer_tick_ms,
};
