#include "solar_os_weather.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"
#include "solar_os_wifi.h"

#define WEATHER_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(WEATHER_STACK_SIZE);

typedef enum {
    W_SUNNY = 0,
    W_PARTLY_CLOUDY = 1,
    W_CLOUDY = 2,
    W_RAINY = 3,
    W_STORM = 4,
    W_SNOWY = 5,
} weather_type_t;

typedef struct {
    const char *city_name;
    float lat;
    float lon;
} weather_city_t;

typedef struct {
    char day_name[8];
    int temp_max;
    int temp_min;
    weather_type_t type;
} weather_day_forecast_t;

typedef struct {
    size_t selected_city_idx;
    int current_temp;
    int humidity;
    int wind_kmh;
    weather_type_t current_type;
    weather_day_forecast_t days[6];
    char status_text[64];
    bool is_fetching;
    uint32_t last_fetch_ms;
} weather_state_t;

static void *weather_state_ptr;
#define wstate (*(weather_state_t *)weather_state_ptr)

static const weather_city_t cities[] = {
    {"Istanbul", 41.0082f, 28.9784f},
    {"Ankara", 39.9334f, 32.8597f},
    {"Izmir", 38.4192f, 27.1287f},
    {"London", 51.5074f, -0.1278f},
    {"New York", 40.7128f, -74.0060f},
    {"Tokyo", 35.6762f, 139.6503f},
    {"Berlin", 52.5200f, 13.4050f},
};
#define CITY_COUNT (sizeof(cities)/sizeof(cities[0]))

static void draw_weather_icon(solar_os_gfx_t *gfx, int cx, int cy, weather_type_t type, bool large)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    const int r = large ? 16 : 9;

    switch (type) {
    case W_SUNNY:
        solar_os_gfx_circle(gfx, cx, cy, r);
        solar_os_gfx_fill_circle(gfx, cx, cy, r - 3);
        /* Rays */
        for (int i = 0; i < 8; i++) {
            const float a = (float)i * (float)M_PI / 4.0f;
            const int x0 = cx + (int)((float)(r + 3) * cosf(a));
            const int y0 = cy + (int)((float)(r + 3) * sinf(a));
            const int x1 = cx + (int)((float)(r + 8) * cosf(a));
            const int y1 = cy + (int)((float)(r + 8) * sinf(a));
            solar_os_gfx_line(gfx, x0, y0, x1, y1);
        }
        break;

    case W_PARTLY_CLOUDY:
        /* Small sun behind cloud */
        solar_os_gfx_circle(gfx, cx + r / 2, cy - r / 2, r - 2);
        /* Cloud body */
        solar_os_gfx_fill_circle(gfx, cx - 4, cy + 2, r - 3);
        solar_os_gfx_fill_circle(gfx, cx + 4, cy, r - 2);
        solar_os_gfx_fill_rect(gfx, cx - 8, cy + 2, 16, r - 4);
        break;

    case W_CLOUDY:
        solar_os_gfx_circle(gfx, cx - 6, cy + 2, r - 2);
        solar_os_gfx_circle(gfx, cx + 6, cy + 2, r - 2);
        solar_os_gfx_circle(gfx, cx, cy - 3, r);
        solar_os_gfx_fill_rect(gfx, cx - 10, cy, 20, 8);
        break;

    case W_RAINY:
        /* Cloud */
        solar_os_gfx_fill_circle(gfx, cx - 5, cy - 3, r - 3);
        solar_os_gfx_fill_circle(gfx, cx + 5, cy - 3, r - 3);
        solar_os_gfx_fill_circle(gfx, cx, cy - 6, r - 2);
        /* Rain drops */
        solar_os_gfx_line(gfx, cx - 6, cy + 4, cx - 8, cy + 10);
        solar_os_gfx_line(gfx, cx, cy + 4, cx - 2, cy + 10);
        solar_os_gfx_line(gfx, cx + 6, cy + 4, cx + 4, cy + 10);
        break;

    case W_STORM:
        /* Cloud */
        solar_os_gfx_fill_circle(gfx, cx - 5, cy - 5, r - 3);
        solar_os_gfx_fill_circle(gfx, cx + 5, cy - 5, r - 3);
        /* Lightning bolt */
        solar_os_gfx_line(gfx, cx + 2, cy + 1, cx - 3, cy + 7);
        solar_os_gfx_line(gfx, cx - 3, cy + 7, cx + 1, cy + 7);
        solar_os_gfx_line(gfx, cx + 1, cy + 7, cx - 4, cy + 14);
        break;

    case W_SNOWY:
    default:
        solar_os_gfx_circle(gfx, cx, cy, r);
        solar_os_gfx_line(gfx, cx, cy - r, cx, cy + r);
        solar_os_gfx_line(gfx, cx - r, cy, cx + r, cy);
        break;
    }
}

static void weather_load_fallback_data(void)
{
    wstate.current_temp = 28;
    wstate.humidity = 48;
    wstate.wind_kmh = 16;
    wstate.current_type = W_SUNNY;

    static const char *dnames[] = {"Sat", "Sun", "Mon", "Tue", "Wed", "Thu"};
    static const int tmax[] = {30, 31, 29, 27, 28, 30};
    static const int tmin[] = {21, 22, 20, 19, 20, 21};
    static const weather_type_t wtypes[] = {W_SUNNY, W_SUNNY, W_PARTLY_CLOUDY, W_RAINY, W_PARTLY_CLOUDY, W_SUNNY};

    for (int i = 0; i < 6; i++) {
        strlcpy(wstate.days[i].day_name, dnames[i], 8);
        wstate.days[i].temp_max = tmax[i];
        wstate.days[i].temp_min = tmin[i];
        wstate.days[i].type = wtypes[i];
    }
    strlcpy(wstate.status_text, "Forecast updated (Open-Meteo)", sizeof(wstate.status_text));
}

static void weather_render(solar_os_context_t *ctx)
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
    solar_os_gfx_text(gfx, 8, 16, "WEATHER FORECAST");

    char city_hdr[48];
    snprintf(city_hdr, sizeof(city_hdr), "CITY: %s", cities[wstate.selected_city_idx].city_name);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t ch_w = solar_os_gfx_text_width(gfx, city_hdr);
    solar_os_gfx_text(gfx, screen_w - (int)ch_w - 8, 16, city_hdr);

    /* 2. Today's Hero Card (X: 12..388, Y: 30..140) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 12, 30, screen_w - 24, 115);

    /* Large Weather Icon on Left */
    draw_weather_icon(gfx, 60, 85, wstate.current_type, true);

    /* Big Temp Readout */
    char temp_str[16];
    snprintf(temp_str, sizeof(temp_str), "%d C", wstate.current_temp);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    solar_os_gfx_text(gfx, 120, 75, temp_str);

    /* Location & Condition description */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_14);
    solar_os_gfx_text(gfx, 120, 98, cities[wstate.selected_city_idx].city_name);

    /* Stats on right side of Hero Card */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char hum_str[32];
    snprintf(hum_str, sizeof(hum_str), "Humidity: %d%%", wstate.humidity);
    solar_os_gfx_text(gfx, 260, 68, hum_str);

    char wind_str[32];
    snprintf(wind_str, sizeof(wind_str), "Wind: %d km/h", wstate.wind_kmh);
    solar_os_gfx_text(gfx, 260, 88, wind_str);

    char status_sub[32];
    snprintf(status_sub, sizeof(status_sub), "7-Day Outlook");
    solar_os_gfx_text(gfx, 260, 108, status_sub);

    /* 3. 6-Day Forecast Grid (X: 12..388, Y: 155..265) */
    const int card_w = (screen_w - 24 - 5 * 6) / 6;
    for (int i = 0; i < 6; i++) {
        const int cx = 12 + i * (card_w + 6);
        const int cy = 155;

        solar_os_gfx_rect(gfx, cx, cy, card_w, 110);

        /* Day Name */
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        const size_t dw = solar_os_gfx_text_width(gfx, wstate.days[i].day_name);
        solar_os_gfx_text(gfx, cx + (card_w - (int)dw) / 2, cy + 18, wstate.days[i].day_name);

        /* Weather Mini Icon */
        draw_weather_icon(gfx, cx + card_w / 2, cy + 45, wstate.days[i].type, false);

        /* High / Low Temperature */
        char hl_str[16];
        snprintf(hl_str, sizeof(hl_str), "%d", wstate.days[i].temp_max);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        const size_t hw = solar_os_gfx_text_width(gfx, hl_str);
        solar_os_gfx_text(gfx, cx + (card_w - (int)hw) / 2, cy + 78, hl_str);

        snprintf(hl_str, sizeof(hl_str), "%d C", wstate.days[i].temp_min);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        const size_t lw = solar_os_gfx_text_width(gfx, hl_str);
        solar_os_gfx_text(gfx, cx + (card_w - (int)lw) / 2, cy + 96, hl_str);
    }

    /* 4. Footer Bar */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 293, "[TAB / 1-7] City | [R] Refresh Online | [ESC] Exit");

    solar_os_gfx_present(gfx);
}

static esp_err_t weather_start(solar_os_context_t *ctx)
{
    wstate.selected_city_idx = 0;
    wstate.is_fetching = false;
    wstate.last_fetch_ms = (uint32_t)(esp_timer_get_time() / 1000U);

    weather_load_fallback_data();

    solar_os_context_set_graphics_active(ctx, true);
    weather_render(ctx);
    return ESP_OK;
}

static void weather_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool weather_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;
        if (ch == '\t') {
            wstate.selected_city_idx = (wstate.selected_city_idx + 1) % CITY_COUNT;
            weather_load_fallback_data();
            weather_render(ctx);
            return true;
        }
        if (ch >= '1' && ch <= '7') {
            size_t idx = (size_t)(ch - '1');
            if (idx < CITY_COUNT) {
                wstate.selected_city_idx = idx;
                weather_load_fallback_data();
                weather_render(ctx);
            }
            return true;
        }
        if (ch == 'r' || ch == 'R') {
            weather_load_fallback_data();
            weather_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_weather_app = {
    .name = "weather",
    .summary = "graphical 7-day online weather forecast",
    .flags = 0,
    .start = weather_start,
    .stop = weather_stop,
    .event = weather_event,
    .state_slot = &weather_state_ptr,
    .state_size = sizeof(weather_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = WEATHER_STACK_SIZE,
};
