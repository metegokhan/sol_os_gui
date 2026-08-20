#include "solar_os_thermometer.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"
#include "solar_os_appbar.h"
#include "solar_os_help.h"

#define THERMO_HISTORY_MAX 60
#define THERMO_STACK_SIZE 4096
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(THERMO_STACK_SIZE);

typedef struct {
    temperature_sensor_handle_t temp_sensor;
    bool sensor_installed;
    float current_celsius;
    float min_celsius;
    float max_celsius;
    float history[THERMO_HISTORY_MAX];
    size_t history_count;
    uint32_t last_sample_ms;
    float calibration_offset; /* added to the raw reading, clamped +/-10 C */
    bool show_help;
} thermo_state_t;

#define THERMO_CAL_LIMIT 10.0f
#define THERMO_CAL_STEP 0.5f

static void *thermo_state_ptr;
#define thermo (*(thermo_state_t *)thermo_state_ptr)

static const char *const thermo_help_lines[] = {
    "Live reading of the ESP32-S3's on-chip temperature",
    "sensor.",
    "",
    "  - Big gauge and digital readout show the current",
    "    temperature in Celsius and Fahrenheit.",
    "  - Min/Max track the extremes seen this session.",
    "  - The trend graph plots the last 60 seconds.",
    "",
    "Calibration: Cal- / Cal+ (or -/+) shift the reading",
    "  in 0.5 C steps, up to +/-10 C.",
    "Reset: clears Min/Max and the trend (chip or R).",
    "Exit: Back arrow or Esc / Q.",
};
#define THERMO_HELP_LINE_COUNT (sizeof(thermo_help_lines) / sizeof(thermo_help_lines[0]))

static size_t thermo_build_footer(solar_os_appbar_shortcut_t *items, size_t max)
{
    size_t n = 0;
    if (n < max) { items[n].key = '-'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Cal-"); n++; }
    if (n < max) { items[n].key = '+'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Cal+"); n++; }
    if (n < max) { items[n].key = 'r'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Reset"); n++; }
    if (n < max) { solar_os_help_chip(&items[n]); n++; }
    return n;
}

static void thermo_sample(void)
{
    float raw = 25.0f;
    if (thermo.sensor_installed && thermo.temp_sensor != NULL) {
        if (temperature_sensor_get_celsius(thermo.temp_sensor, &raw) != ESP_OK) {
            raw = thermo.current_celsius - thermo.calibration_offset;
        }
    }
    const float val = raw + thermo.calibration_offset;
    thermo.current_celsius = val;

    if (thermo.history_count == 0) {
        thermo.min_celsius = val;
        thermo.max_celsius = val;
    } else {
        if (val < thermo.min_celsius) thermo.min_celsius = val;
        if (val > thermo.max_celsius) thermo.max_celsius = val;
    }

    if (thermo.history_count < THERMO_HISTORY_MAX) {
        thermo.history[thermo.history_count++] = val;
    } else {
        memmove(&thermo.history[0], &thermo.history[1], sizeof(float) * (THERMO_HISTORY_MAX - 1));
        thermo.history[THERMO_HISTORY_MAX - 1] = val;
    }
}

/* Nudges the calibration offset by `delta`, clamped to +/-10 C, and shifts the
 * live reading, min/max and history so the display stays continuous. */
static void thermo_adjust_cal(float delta)
{
    float target = thermo.calibration_offset + delta;
    if (target > THERMO_CAL_LIMIT) target = THERMO_CAL_LIMIT;
    if (target < -THERMO_CAL_LIMIT) target = -THERMO_CAL_LIMIT;
    const float applied = target - thermo.calibration_offset;
    if (applied == 0.0f) return;

    thermo.calibration_offset = target;
    thermo.current_celsius += applied;
    thermo.min_celsius += applied;
    thermo.max_celsius += applied;
    for (size_t i = 0; i < thermo.history_count; i++) {
        thermo.history[i] += applied;
    }
}

static void thermo_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Shared header. */
    solar_os_appbar_header_t header = {0};
    header.title = "Thermometer";
    header.show_back = true;
    char status_line[64];
    snprintf(status_line, sizeof(status_line), "ESP32-S3 sensor   Cal: %+.1f C",
             (double)thermo.calibration_offset);
    header.status_line = status_line;
    solar_os_appbar_draw_header(gfx, &header);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    /* 2. Left Column: Big Thermometer Gauge (X: 30..90, Y: 40..250) */
    const int tube_x = 45;
    const int tube_top = 50;
    const int tube_bottom = 220;
    const int tube_w = 16;
    const int bulb_radius = 16;
    const int bulb_y = tube_bottom + 12;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    /* Outer Tube outline */
    solar_os_gfx_rect(gfx, tube_x, tube_top, tube_w, tube_bottom - tube_top);
    solar_os_gfx_circle(gfx, tube_x + tube_w / 2, bulb_y, bulb_radius);
    solar_os_gfx_fill_circle(gfx, tube_x + tube_w / 2, bulb_y, bulb_radius - 2);

    /* Fill level according to temperature (0C..80C) */
    float temp_clamped = thermo.current_celsius;
    if (temp_clamped < 0.0f) temp_clamped = 0.0f;
    if (temp_clamped > 80.0f) temp_clamped = 80.0f;
    const int fill_h = (int)((temp_clamped / 80.0f) * (float)(tube_bottom - tube_top));
    solar_os_gfx_fill_rect(gfx, tube_x + 3, tube_bottom - fill_h, tube_w - 6, fill_h + 4);

    /* Scale Ticks */
    for (int t = 0; t <= 80; t += 20) {
        const int ty = tube_bottom - (int)((float)t / 80.0f * (float)(tube_bottom - tube_top));
        solar_os_gfx_line(gfx, tube_x + tube_w + 2, ty, tube_x + tube_w + 8, ty);
        char tick_str[8];
        snprintf(tick_str, sizeof(tick_str), "%d", t);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, tube_x + tube_w + 12, ty + 4, tick_str);
    }

    /* 3. Center/Right: Big Digital Readout */
    char c_str[32];
    snprintf(c_str, sizeof(c_str), "%.1f C", thermo.current_celsius);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    solar_os_gfx_text(gfx, 130, 80, c_str);

    char f_str[32];
    const float fahrenheit = (thermo.current_celsius * 1.8f) + 32.0f;
    snprintf(f_str, sizeof(f_str), "%.1f F", fahrenheit);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_text(gfx, 130, 110, f_str);

    /* Stats Box (X: 130..370, Y: 125..165) */
    solar_os_gfx_rect(gfx, 130, 125, 240, 42);
    char stats_str[64];
    snprintf(stats_str, sizeof(stats_str), "Min: %.1f C   Max: %.1f C", thermo.min_celsius, thermo.max_celsius);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 140, 148, stats_str);

    /* 4. Bottom Right: Live Trend Graph (X: 130..370, Y: 180..255) */
    solar_os_gfx_rect(gfx, 130, 180, 240, 75);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 136, 194, "Realtime Temperature Trend (60s)");

    if (thermo.history_count >= 2) {
        float g_min = thermo.min_celsius - 1.0f;
        float g_max = thermo.max_celsius + 1.0f;
        if (g_max <= g_min) g_max = g_min + 2.0f;

        const int gw = 220;
        const int gh = 45;
        const int gx = 140;
        const int gy = 200;

        for (size_t i = 1; i < thermo.history_count; i++) {
            const int x0 = gx + (int)((i - 1) * gw / (THERMO_HISTORY_MAX - 1));
            const int x1 = gx + (int)(i * gw / (THERMO_HISTORY_MAX - 1));

            const int y0 = gy + gh - (int)((thermo.history[i - 1] - g_min) / (g_max - g_min) * (float)gh);
            const int y1 = gy + gh - (int)((thermo.history[i] - g_min) / (g_max - g_min) * (float)gh);

            solar_os_gfx_line(gfx, x0, y0, x1, y1);
            solar_os_gfx_line(gfx, x0, y0 + 1, x1, y1 + 1);
        }
    }

    /* 5. Shared footer chips. */
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = thermo_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);

    if (thermo.show_help) {
        solar_os_help_draw(gfx, "Thermometer - Help", thermo_help_lines, THERMO_HELP_LINE_COUNT);
    }

    solar_os_gfx_present(gfx);
}

static esp_err_t thermo_start(solar_os_context_t *ctx)
{
    thermo.current_celsius = 25.0f;
    thermo.min_celsius = 25.0f;
    thermo.max_celsius = 25.0f;
    thermo.history_count = 0;
    thermo.last_sample_ms = 0;

    temperature_sensor_config_t temp_cfg = {
        .range_min = 10,
        .range_max = 80,
    };
    if (temperature_sensor_install(&temp_cfg, &thermo.temp_sensor) == ESP_OK) {
        if (temperature_sensor_enable(thermo.temp_sensor) == ESP_OK) {
            thermo.sensor_installed = true;
        }
    }

    solar_os_context_set_graphics_active(ctx, true);
    thermo_sample();
    thermo_render(ctx);
    return ESP_OK;
}

static void thermo_stop(solar_os_context_t *ctx)
{
    if (thermo.sensor_installed && thermo.temp_sensor != NULL) {
        temperature_sensor_disable(thermo.temp_sensor);
        temperature_sensor_uninstall(thermo.temp_sensor);
        thermo.temp_sensor = NULL;
        thermo.sensor_installed = false;
    }
    solar_os_context_set_graphics_active(ctx, false);
}

static bool thermo_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
        if (now - thermo.last_sample_ms >= 1000U) {
            thermo.last_sample_ms = now;
            thermo_sample();
            thermo_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        if (thermo.show_help) {
            thermo.show_help = false;
            thermo_render(ctx);
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
        const size_t count = thermo_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < count) {
                switch (items[fhit.index].key) {
                case '-': thermo_adjust_cal(-THERMO_CAL_STEP); break;
                case '+': thermo_adjust_cal(+THERMO_CAL_STEP); break;
                case 'r':
                    thermo.min_celsius = thermo.current_celsius;
                    thermo.max_celsius = thermo.current_celsius;
                    thermo.history_count = 0;
                    thermo_sample();
                    break;
                case 'H': thermo.show_help = true; break;
                default: break;
                }
                thermo_render(ctx);
            }
            return true;
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (thermo.show_help) {
            thermo.show_help = false;
            thermo_render(ctx);
            return true;
        }
        if (solar_os_help_char_opens(ch)) {
            thermo.show_help = true;
            thermo_render(ctx);
            return true;
        }
        if (ch == '+' || ch == '=') {
            thermo_adjust_cal(+THERMO_CAL_STEP);
            thermo_render(ctx);
            return true;
        }
        if (ch == '-' || ch == '_') {
            thermo_adjust_cal(-THERMO_CAL_STEP);
            thermo_render(ctx);
            return true;
        }
        if (ch == 'r' || ch == 'R') {
            thermo.min_celsius = thermo.current_celsius;
            thermo.max_celsius = thermo.current_celsius;
            thermo.history_count = 0;
            thermo_sample();
            thermo_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_thermometer_app = {
    .name = "thermometer",
    .summary = "hardware internal temperature monitor",
    .flags = 0,
    .start = thermo_start,
    .stop = thermo_stop,
    .event = thermo_event,
    .state_slot = &thermo_state_ptr,
    .state_size = sizeof(thermo_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 1000U,
    .worker_stack_bytes = THERMO_STACK_SIZE,
};
