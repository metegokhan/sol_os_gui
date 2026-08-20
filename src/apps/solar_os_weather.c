#include "solar_os_weather.h"

#include <math.h>
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
#include "solar_os_http_client.h"
#include "solar_os_wifi.h"
#include "solar_os_weather_icons.h"

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
    weather_type_t type;   /* representative current condition */
    int base_temp;         /* representative current temperature (C) */
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

    uint32_t anim_frame;
    bool city_picker;
    int city_scroll;
    bool show_help;
} weather_state_t;

static void *weather_state_ptr;
#define wstate (*(weather_state_t *)weather_state_ptr)

/* An expanded set of cities with a representative condition so the animated
 * hero shows real variety (rain, snow, storm, and hot/sunny for flames). */
static const weather_city_t cities[] = {
    {"Istanbul",  41.0082f,  28.9784f, W_RAINY,          17},
    {"Ankara",    39.9334f,  32.8597f, W_PARTLY_CLOUDY,  14},
    {"Izmir",     38.4192f,  27.1287f, W_SUNNY,          26},
    {"Antalya",   36.8969f,  30.7133f, W_SUNNY,          30},
    {"London",    51.5074f,  -0.1278f, W_RAINY,          12},
    {"Paris",     48.8566f,   2.3522f, W_CLOUDY,         14},
    {"Berlin",    52.5200f,  13.4050f, W_CLOUDY,         13},
    {"Rome",      41.9028f,  12.4964f, W_SUNNY,          28},
    {"Moscow",    55.7558f,  37.6173f, W_SNOWY,          -4},
    {"Oslo",      59.9139f,  10.7522f, W_SNOWY,          -1},
    {"Reykjavik", 64.1466f, -21.9426f, W_SNOWY,           2},
    {"New York",  40.7128f, -74.0060f, W_CLOUDY,         16},
    {"Dubai",     25.2048f,  55.2708f, W_SUNNY,          41},
    {"Cairo",     30.0444f,  31.2357f, W_SUNNY,          36},
    {"Singapore",  1.3521f, 103.8198f, W_STORM,          31},
    {"Tokyo",     35.6762f, 139.6503f, W_PARTLY_CLOUDY,  20},
    {"Sydney",   -33.8688f, 151.2093f, W_SUNNY,          24},
};
#define CITY_COUNT (sizeof(cities)/sizeof(cities[0]))

static const char *const weather_help_lines[] = {
    "A 6-day forecast with an animated sky.",
    "",
    "  - City: open the list and tap a city (or 1-9, or",
    "    Left/Right / Tab to cycle).",
    "  - The hero card animates the current condition:",
    "    rain streaks, drifting snow, storm bolts, or",
    "    rising flames when it is very hot.",
    "  - Refresh reloads the forecast.",
    "",
    "Exit: Back arrow or Esc / Q.",
};
#define WEATHER_HELP_LINE_COUNT (sizeof(weather_help_lines) / sizeof(weather_help_lines[0]))

/* --------------------------------------------------------------- icon */

/* Blits a crisp monochrome weather glyph centered at (cx,cy). `large` picks
 * the 40px hero icon; otherwise the 20px forecast icon. */
static void draw_weather_icon(solar_os_gfx_t *gfx, int cx, int cy, weather_type_t type, bool large)
{
    const int t = (int)type;
    if (t < 0 || t > 5) return;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    if (large) {
        solar_os_gfx_bitmap(gfx, cx - WICON_48_W / 2, cy - WICON_48_H / 2,
                            WICON_48_W, WICON_48_H, wicons_48[t]);
    } else {
        solar_os_gfx_bitmap(gfx, cx - WICON_24_W / 2, cy - WICON_24_H / 2,
                            WICON_24_W, WICON_24_H, wicons_24[t]);
    }
}

/* --------------------------------------------------------------- animation */

/* Draws the moving weather effect over a rectangular region. */
static void weather_draw_anim(solar_os_gfx_t *gfx, int rx, int ry, int rw, int rh,
                              weather_type_t type, int temp, uint32_t frame)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    const uint32_t f = frame;

    if (type == W_RAINY || type == W_STORM) {
        for (int i = 0; i < 22; i++) {
            const int x = rx + (int)((uint32_t)(i * 41u) % (uint32_t)rw);
            const int y = ry + (int)((uint32_t)(i * 27u + f * 9u) % (uint32_t)rh);
            solar_os_gfx_line(gfx, x, y, x - 3, y + 8);
            solar_os_gfx_line(gfx, x + 1, y, x - 2, y + 8);
        }
        if (type == W_STORM && (f % 11u) < 2u) {
            const int lx = rx + rw / 2 + 30;
            solar_os_gfx_line(gfx, lx, ry + 8, lx - 8, ry + 24);
            solar_os_gfx_line(gfx, lx - 8, ry + 24, lx - 2, ry + 24);
            solar_os_gfx_line(gfx, lx - 2, ry + 24, lx - 12, ry + 44);
            solar_os_gfx_line(gfx, lx + 1, ry + 8, lx - 7, ry + 24);
        }
    } else if (type == W_SNOWY) {
        for (int i = 0; i < 20; i++) {
            const int drift = (int)(3.0f * sinf((float)f * 0.15f + (float)i));
            const int x = rx + (int)((uint32_t)(i * 33u + (uint32_t)(drift + 8)) % (uint32_t)rw);
            const int y = ry + (int)((uint32_t)(i * 21u + f * 4u) % (uint32_t)rh);
            solar_os_gfx_fill_circle(gfx, x, y, 2);
        }
    } else if ((type == W_SUNNY || type == W_PARTLY_CLOUDY) && temp >= 33) {
        /* Rising flames along the bottom of the region when it is very hot. */
        const int base_y = ry + rh - 4;
        for (int k = 0; k < 5; k++) {
            const int fx = rx + 24 + k * 34;
            const int hgt = 22 + (int)(8.0f * sinf((float)f * 0.4f + (float)k * 1.3f));
            const int sway = (int)(4.0f * sinf((float)f * 0.5f + (float)k));
            const solar_os_gfx_point_t flame[3] = {
                { fx - 9, base_y },
                { fx + 9, base_y },
                { fx + sway, base_y - hgt },
            };
            solar_os_gfx_fill_polygon(gfx, flame, 3);
            /* inner lick */
            const solar_os_gfx_point_t inner[3] = {
                { fx - 4, base_y },
                { fx + 4, base_y },
                { fx + sway / 2, base_y - hgt / 2 },
            };
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_polygon(gfx, inner, 3);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }
    }
}

/* --------------------------------------------------------------- data */

static const char *weather_type_name(weather_type_t t)
{
    switch (t) {
    case W_SUNNY:         return "Clear / Sunny";
    case W_PARTLY_CLOUDY: return "Partly Cloudy";
    case W_CLOUDY:        return "Cloudy";
    case W_RAINY:         return "Rainy";
    case W_STORM:         return "Thunderstorm";
    case W_SNOWY:         return "Snow";
    default:              return "";
    }
}

static void weather_load_city_data(void)
{
    const weather_city_t *c = &cities[wstate.selected_city_idx];
    wstate.current_type = c->type;
    wstate.current_temp = c->base_temp;
    wstate.humidity = (c->type == W_RAINY || c->type == W_STORM) ? 82 :
                      (c->type == W_SNOWY) ? 70 : 45;
    wstate.wind_kmh = 8 + (int)(wstate.selected_city_idx % 5) * 4;

    static const char *dnames[] = {"Sat", "Sun", "Mon", "Tue", "Wed", "Thu"};
    for (int i = 0; i < 6; i++) {
        strlcpy(wstate.days[i].day_name, dnames[i], 8);
        wstate.days[i].temp_max = c->base_temp + ((i * 2) % 5) - 1;
        wstate.days[i].temp_min = c->base_temp - 6 + (i % 3);
        /* Ease from the current condition toward calmer days. */
        wstate.days[i].type = (i == 0) ? c->type :
            (i % 3 == 0 ? c->type :
             i % 3 == 1 ? W_PARTLY_CLOUDY : W_CLOUDY);
    }
    snprintf(wstate.status_text, sizeof(wstate.status_text), "%s", weather_type_name(c->type));
}

/* --------------------------------------------------------------- live fetch */

static weather_type_t wmo_to_type(int code)
{
    if (code == 0) return W_SUNNY;
    if (code == 1 || code == 2) return W_PARTLY_CLOUDY;
    if (code == 3 || code == 45 || code == 48) return W_CLOUDY;
    if (code >= 51 && code <= 67) return W_RAINY;
    if (code >= 71 && code <= 77) return W_SNOWY;
    if (code >= 80 && code <= 82) return W_RAINY;
    if (code >= 85 && code <= 86) return W_SNOWY;
    if (code >= 95) return W_STORM;
    return W_CLOUDY;
}

/* Sakamoto's algorithm -> 3-letter weekday for a Gregorian date. */
static const char *weather_wday(int y, int m, int d)
{
    static const char *const names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 1 || m > 12) return "---";
    if (m < 3) y -= 1;
    const int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return names[(w % 7 + 7) % 7];
}

/* Reads a JSON number appearing right after `key` in `sect`. */
static bool weather_json_num(const char *sect, const char *key, double *out)
{
    const char *p = strstr(sect, key);
    if (p == NULL) return false;
    p = strchr(p, ':');
    if (p == NULL) return false;
    *out = atof(p + 1);
    return true;
}

static int weather_read_num_array(const char *after_bracket, double *out, int maxn)
{
    const char *p = after_bracket;
    int n = 0;
    while (n < maxn && *p) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;
        out[n++] = atof(p);
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ']') break;
    }
    return n;
}

typedef struct { char *buf; size_t len, cap; } weather_fetch_ctx_t;

static esp_err_t weather_http_evt(const solar_os_http_event_t *e, void *ud)
{
    if (e != NULL && e->type == SOLAR_OS_HTTP_EVENT_DATA && e->data != NULL && e->data_len > 0) {
        weather_fetch_ctx_t *c = (weather_fetch_ctx_t *)ud;
        size_t n = e->data_len;
        if (c->len + n > c->cap - 1) n = c->cap - 1 - c->len;
        if (n > 0) {
            memcpy(c->buf + c->len, e->data, n);
            c->len += n;
            c->buf[c->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool weather_parse(const char *buf)
{
    const char *cur = strstr(buf, "\"current\"");
    const char *daily = strstr(buf, "\"daily\"");
    if (cur == NULL) return false;

    double v;
    if (!weather_json_num(cur, "\"temperature_2m\"", &v)) return false;
    wstate.current_temp = (int)lround(v);
    if (weather_json_num(cur, "\"relative_humidity_2m\"", &v)) wstate.humidity = (int)lround(v);
    if (weather_json_num(cur, "\"wind_speed_10m\"", &v)) wstate.wind_kmh = (int)lround(v);
    if (weather_json_num(cur, "\"weather_code\"", &v)) wstate.current_type = wmo_to_type((int)v);

    if (daily != NULL) {
        double codes[6] = {0}, mx[6] = {0}, mn[6] = {0};
        const char *p;
        if ((p = strstr(daily, "\"weather_code\":[")) != NULL)
            weather_read_num_array(strchr(p, '[') + 1, codes, 6);
        if ((p = strstr(daily, "\"temperature_2m_max\":[")) != NULL)
            weather_read_num_array(strchr(p, '[') + 1, mx, 6);
        if ((p = strstr(daily, "\"temperature_2m_min\":[")) != NULL)
            weather_read_num_array(strchr(p, '[') + 1, mn, 6);

        for (int i = 0; i < 6; i++) {
            wstate.days[i].type = wmo_to_type((int)codes[i]);
            wstate.days[i].temp_max = (int)lround(mx[i]);
            wstate.days[i].temp_min = (int)lround(mn[i]);
        }
        const char *tp = strstr(daily, "\"time\":[");
        if (tp != NULL) {
            p = strchr(tp, '[') + 1;
            for (int i = 0; i < 6 && p != NULL && *p; i++) {
                while (*p == ' ' || *p == ',' || *p == '"') p++;
                int Y, M, D;
                if (sscanf(p, "%d-%d-%d", &Y, &M, &D) == 3) {
                    strlcpy(wstate.days[i].day_name, weather_wday(Y, M, D), sizeof(wstate.days[i].day_name));
                }
                p = strchr(p, ',');
                if (p) p++;
            }
        }
    }

    snprintf(wstate.status_text, sizeof(wstate.status_text), "Live: %s",
             weather_type_name(wstate.current_type));
    return true;
}

/* Blocking Open-Meteo fetch for the selected city. Returns false (and leaves
 * state untouched) when offline or on any error. */
static bool weather_fetch(void)
{
    solar_os_wifi_status_t wst;
    solar_os_wifi_get_status(&wst);
    if (!wst.connected || !wst.has_ip) return false;

    static char buf[4096];
    weather_fetch_ctx_t fc = { buf, 0, sizeof(buf) };
    buf[0] = '\0';

    const weather_city_t *city = &cities[wstate.selected_city_idx];
    char url[352];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min"
             "&forecast_days=6&timezone=auto",
             (double)city->lat, (double)city->lon);

    solar_os_http_request_options_t opt = {0};
    opt.url = url;
    opt.method = SOLAR_OS_HTTP_METHOD_GET;
    opt.follow_redirects = true;
    opt.timeout_ms = 8000;
    opt.deadline_ms = 12000;
    opt.user_agent = "SolarOS-Weather";
    opt.event_handler = weather_http_evt;
    opt.user_data = &fc;

    solar_os_http_request_t *req = NULL;
    if (solar_os_http_request_create(&opt, &req) != ESP_OK) return false;
    solar_os_http_response_t resp = {0};
    const esp_err_t err = solar_os_http_request_perform(req, &resp);
    solar_os_http_request_destroy(req);

    if (err != ESP_OK || resp.status_code < 200 || resp.status_code >= 300 || fc.len < 20) {
        return false;
    }
    return weather_parse(buf);
}

/* --------------------------------------------------------------- footer */

static size_t weather_build_footer(solar_os_appbar_shortcut_t *items, size_t max)
{
    size_t n = 0;
    if (n < max) { items[n].key = 'c'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "City"); n++; }
    if (n < max) { items[n].key = 'r'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Refresh"); n++; }
    if (n < max) { solar_os_help_chip(&items[n]); n++; }
    return n;
}

/* --------------------------------------------------------------- city picker */

#define WEATHER_PICK_ROW_H 20

static void weather_pick_box(solar_os_gfx_t *gfx, int *bx, int *by, int *bw, int *bh)
{
    const int w = (int)solar_os_gfx_width(gfx);
    const int h = (int)solar_os_gfx_height(gfx);
    *bx = 30; *by = 16; *bw = w - 60; *bh = h - 32;
}

static void weather_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* Header */
    solar_os_appbar_header_t header = {0};
    header.title = "Weather";
    header.show_back = true;
    char status_line[80];
    snprintf(status_line, sizeof(status_line), "%s   %s",
             cities[wstate.selected_city_idx].city_name, wstate.status_text);
    header.status_line = status_line;
    solar_os_appbar_draw_header(gfx, &header);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    const int top = solar_os_appbar_header_height(gfx) + solar_os_appbar_status_line_height(gfx) + 2;

    /* Hero card with animated sky */
    const int hx = 12, hy = top, hw = screen_w - 24, hh = 116;
    solar_os_gfx_rect(gfx, hx, hy, hw, hh);
    weather_draw_anim(gfx, hx + 2, hy + 2, hw - 4, hh - 4,
                      wstate.current_type, wstate.current_temp, wstate.anim_frame);

    /* Big icon + readout (drawn over the animation) */
    draw_weather_icon(gfx, hx + 46, hy + 54, wstate.current_type, true);

    char temp_str[16];
    snprintf(temp_str, sizeof(temp_str), "%d C", wstate.current_temp);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    solar_os_gfx_text(gfx, hx + 96, hy + 46, temp_str);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_14);
    solar_os_gfx_text(gfx, hx + 96, hy + 70, cities[wstate.selected_city_idx].city_name);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, hx + 96, hy + 88, weather_type_name(wstate.current_type));

    char stat[32];
    snprintf(stat, sizeof(stat), "Humidity %d%%", wstate.humidity);
    solar_os_gfx_text(gfx, hx + hw - 116, hy + 70, stat);
    snprintf(stat, sizeof(stat), "Wind %d km/h", wstate.wind_kmh);
    solar_os_gfx_text(gfx, hx + hw - 116, hy + 88, stat);

    /* 6-day forecast row */
    const int fy = hy + hh + 8;
    const int fh = 108;
    const int card_w = (screen_w - 24 - 5 * 6) / 6;
    for (int i = 0; i < 6; i++) {
        const int cx = 12 + i * (card_w + 6);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, cx, fy, card_w, fh);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        const size_t dw = solar_os_gfx_text_width(gfx, wstate.days[i].day_name);
        solar_os_gfx_text(gfx, cx + (card_w - (int)dw) / 2, fy + 16, wstate.days[i].day_name);

        draw_weather_icon(gfx, cx + card_w / 2, fy + 44, wstate.days[i].type, false);

        char hl[16];
        snprintf(hl, sizeof(hl), "%d", wstate.days[i].temp_max);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        size_t tw = solar_os_gfx_text_width(gfx, hl);
        solar_os_gfx_text(gfx, cx + (card_w - (int)tw) / 2, fy + 78, hl);
        snprintf(hl, sizeof(hl), "%d C", wstate.days[i].temp_min);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        tw = solar_os_gfx_text_width(gfx, hl);
        solar_os_gfx_text(gfx, cx + (card_w - (int)tw) / 2, fy + 96, hl);
    }

    /* Footer chips */
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = weather_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);

    /* City picker overlay */
    if (wstate.city_picker) {
        int bx, by, bw, bh;
        weather_pick_box(gfx, &bx, &by, &bw, &bh);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, bx, by, bw, bh);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, bx, by, bw, bh);
        solar_os_gfx_fill_rect(gfx, bx + 2, by + 2, bw - 4, 20);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, bx + 10, by + 16, "Select City");
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

        const int rows = (bh - 30) / WEATHER_PICK_ROW_H;
        for (int r = 0; r < rows; r++) {
            const size_t idx = (size_t)wstate.city_scroll + (size_t)r;
            if (idx >= CITY_COUNT) break;
            const int ry = by + 26 + r * WEATHER_PICK_ROW_H;
            if (idx == wstate.selected_city_idx) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, bx + 3, ry, bw - 6, WEATHER_PICK_ROW_H);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            }
            char row[40];
            snprintf(row, sizeof(row), "%u. %s", (unsigned)(idx + 1), cities[idx].city_name);
            solar_os_gfx_text(gfx, bx + 12, ry + 14, row);
        }
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, bx + 12, by + bh - 8, "Tap a city  |  scroll for more");
    }

    /* Help overlay */
    if (wstate.show_help) {
        solar_os_help_draw(gfx, "Weather - Help", weather_help_lines, WEATHER_HELP_LINE_COUNT);
    }

    solar_os_gfx_present(gfx);
}

/* --------------------------------------------------------------- lifecycle */

static void weather_refresh(solar_os_context_t *ctx);

static esp_err_t weather_start(solar_os_context_t *ctx)
{
    memset(&wstate, 0, sizeof(wstate));
    wstate.selected_city_idx = 0;
    weather_load_city_data();
    solar_os_context_set_graphics_active(ctx, true);
    weather_refresh(ctx); /* try live data; falls back to sample when offline */
    return ESP_OK;
}

static void weather_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void weather_select_city(solar_os_context_t *ctx, size_t idx)
{
    if (idx >= CITY_COUNT) return;
    wstate.selected_city_idx = idx;
    weather_load_city_data();
    weather_render(ctx);
}

/* Shows a "Fetching..." frame, tries a live Open-Meteo fetch for the current
 * city, and falls back to the built-in sample data when offline. */
static void weather_refresh(solar_os_context_t *ctx)
{
    strlcpy(wstate.status_text, "Fetching...", sizeof(wstate.status_text));
    weather_render(ctx);
    if (!weather_fetch()) {
        weather_load_city_data();
        strlcpy(wstate.status_text, "Offline - sample data", sizeof(wstate.status_text));
    }
    weather_render(ctx);
}

/* Deliberate city change: paint sample instantly, then pull live data. */
static void weather_goto_city(solar_os_context_t *ctx, size_t idx)
{
    if (idx >= CITY_COUNT) return;
    wstate.selected_city_idx = idx;
    weather_load_city_data();
    weather_refresh(ctx);
}

static void weather_scroll_picker(int dir)
{
    const int max_scroll = (int)CITY_COUNT - 1;
    wstate.city_scroll += dir;
    if (wstate.city_scroll < 0) wstate.city_scroll = 0;
    if (wstate.city_scroll > max_scroll) wstate.city_scroll = max_scroll;
}

static bool weather_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_TICK) {
        /* Animate only the live sky (not while an overlay is up). */
        if (!wstate.city_picker && !wstate.show_help) {
            wstate.anim_frame++;
            weather_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        if (wstate.show_help) {
            wstate.show_help = false;
            weather_render(ctx);
            return true;
        }

        if (wstate.city_picker) {
            int bx, by, bw, bh;
            weather_pick_box(gfx, &bx, &by, &bw, &bh);
            if (px >= bx && px < bx + bw && py >= by + 26 && py < by + bh - 10) {
                const int r = (py - (by + 26)) / WEATHER_PICK_ROW_H;
                const size_t idx = (size_t)wstate.city_scroll + (size_t)r;
                if (idx < CITY_COUNT) {
                    wstate.city_picker = false;
                    weather_goto_city(ctx, idx);
                    return true;
                }
            }
            /* tap outside list closes it */
            wstate.city_picker = false;
            weather_render(ctx);
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
        const size_t count = weather_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < count) {
                switch (items[fhit.index].key) {
                case 'c': wstate.city_picker = true; wstate.city_scroll = 0; break;
                case 'r': weather_refresh(ctx); return true;
                case 'H': wstate.show_help = true; break;
                default: break;
                }
                weather_render(ctx);
            }
            return true;
        }

        /* Tap the hero card to open the city list. */
        const int top = solar_os_appbar_header_height(gfx) + solar_os_appbar_status_line_height(gfx) + 2;
        if (py >= top && py < top + 116) {
            wstate.city_picker = true;
            wstate.city_scroll = 0;
            weather_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_SCROLL) {
        if (wstate.city_picker) {
            weather_scroll_picker(event->data.scroll.delta < 0 ? 1 : -1);
            weather_render(ctx);
        } else {
            weather_select_city(ctx,
                (wstate.selected_city_idx + (event->data.scroll.delta < 0 ? 1 : CITY_COUNT - 1)) % CITY_COUNT);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (wstate.show_help) {
            wstate.show_help = false;
            weather_render(ctx);
            return true;
        }
        if (solar_os_help_char_opens(ch)) {
            wstate.show_help = true;
            weather_render(ctx);
            return true;
        }
        if (wstate.city_picker) {
            if (ch == SOLAR_OS_KEY_UP) { weather_scroll_picker(-1); weather_render(ctx); return true; }
            if (ch == SOLAR_OS_KEY_DOWN) { weather_scroll_picker(1); weather_render(ctx); return true; }
            if (ch == SOLAR_OS_KEY_ESCAPE || ch == ' ') { wstate.city_picker = false; weather_render(ctx); return true; }
        }

        if (ch == 'c' || ch == 'C') {
            wstate.city_picker = true; wstate.city_scroll = 0;
            weather_render(ctx);
            return true;
        }
        if (ch == '\t' || ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D') {
            weather_goto_city(ctx, (wstate.selected_city_idx + 1) % CITY_COUNT);
            return true;
        }
        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A') {
            weather_goto_city(ctx, (wstate.selected_city_idx + CITY_COUNT - 1) % CITY_COUNT);
            return true;
        }
        if (ch >= '1' && ch <= '9') {
            weather_goto_city(ctx, (size_t)(ch - '1'));
            return true;
        }
        if (ch == 'r' || ch == 'R') {
            weather_refresh(ctx);
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
    .summary = "animated 6-day weather with many cities",
    .flags = 0,
    .start = weather_start,
    .stop = weather_stop,
    .event = weather_event,
    .state_slot = &weather_state_ptr,
    .state_size = sizeof(weather_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 140U,
    .worker_stack_bytes = WEATHER_STACK_SIZE,
};
