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
} thermo_state_t;

static void *thermo_state_ptr;
#define thermo (*(thermo_state_t *)thermo_state_ptr)

static void thermo_sample(void)
{
    float val = 25.0f;
    if (thermo.sensor_installed && thermo.temp_sensor != NULL) {
        if (temperature_sensor_get_celsius(thermo.temp_sensor, &val) != ESP_OK) {
            val = thermo.current_celsius;
        }
    }
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

static void thermo_render(solar_os_context_t *ctx)
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
    solar_os_gfx_text(gfx, 8, 16, "HARDWARE THERMOMETER");

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, screen_w - 110, 16, "ESP32-S3 TSENS");

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

    /* 5. Footer Navigation Bar */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 293, "[R] Reset Min/Max | [ESC] Exit to Desktop");

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

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;
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
    .worker_stack_bytes = THERMO_STACK_SIZE,
};
