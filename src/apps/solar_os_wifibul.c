#include "solar_os_wifibul.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "solar_os_audio.h"
#include "solar_os_appbar.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_task.h"
#include "solar_os_wifi.h"

#define TAG "wifibul"

#define WIFIBUL_TASK_STACK 6144U
#define WIFIBUL_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define WIFIBUL_TICK_MS 100U
#define WIFIBUL_RESCAN_MS 3000U

/* Matches solar_os_appbar_header_height()/footer_height() for this board's
 * resolution -- body draw functions below address fixed pixel origins, but
 * the actual bars are drawn by the shared appbar component. */
#define WIFIBUL_HEADER_H 22
#define WIFIBUL_FOOTER_H 18

typedef enum {
    WIFIBUL_VIEW_LIST = 0,
    WIFIBUL_VIEW_DETAIL = 1,
    WIFIBUL_VIEW_RADAR = 2,
} wifibul_view_t;

typedef struct {
    /* --- Scan worker (guarded by wifibul_lock) --- */
    TaskHandle_t task;
    volatile bool task_done;
    solar_os_wifi_ap_t staging_results[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t staging_count;
    esp_err_t staging_err;
    bool staging_ready;

    /* --- Main thread state --- */
    bool scanning;
    solar_os_wifi_ap_t results[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t result_count;
    size_t selected;

    wifibul_view_t view;

    /* --- Radar / Tracking --- */
    char tracked_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    uint8_t tracked_bssid[6];
    bool tracked_found;
    int8_t tracked_rssi;
    uint32_t next_rescan_ms;
    uint32_t next_beep_ms;
    bool blink_on;

    uint32_t elapsed_ms;
    bool render_pending;
    char status_message[64];
    uint32_t status_until_ms;
} wifibul_state_t;

static void *wifibul_state_ptr;
#define wifibul (*(wifibul_state_t *)wifibul_state_ptr)

SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("wifibul scan worker spinlock, no app data")
static portMUX_TYPE wifibul_lock = portMUX_INITIALIZER_UNLOCKED;

static void wifibul_render(solar_os_context_t *ctx);
static void wifibul_set_status(const char *message);

/* ---------------------------------------------------------------------
 * Scan Worker Task
 * ------------------------------------------------------------------- */

static void wifibul_scan_worker(void *arg)
{
    (void)arg;
    solar_os_wifi_ap_t local_results[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t found = 0U;
    const esp_err_t err = solar_os_wifi_scan(local_results, SOLAR_OS_WIFI_SCAN_MAX_RESULTS, &found);

    portENTER_CRITICAL(&wifibul_lock);
    if (err == ESP_OK) {
        memcpy(wifibul.staging_results, local_results, sizeof(local_results));
        wifibul.staging_count = found;
    } else {
        wifibul.staging_count = 0U;
    }
    wifibul.staging_err = err;
    wifibul.staging_ready = true;
    wifibul.task_done = true;
    portEXIT_CRITICAL(&wifibul_lock);

    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void wifibul_start_scan(void)
{
    if (wifibul.scanning) return;

    portENTER_CRITICAL(&wifibul_lock);
    wifibul.staging_ready = false;
    wifibul.task_done = false;
    portEXIT_CRITICAL(&wifibul_lock);

    if (wifibul.task != NULL) {
        vTaskResume(wifibul.task);
        wifibul.scanning = true;
        wifibul.render_pending = true;
        return;
    }

    const BaseType_t created = solar_os_task_create_pinned_internal(
        wifibul_scan_worker, "wifibul_scan", WIFIBUL_TASK_STACK, NULL, WIFIBUL_TASK_PRIORITY,
        &wifibul.task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created == pdPASS) {
        wifibul.scanning = true;
        wifibul.render_pending = true;
    } else {
        wifibul.task = NULL;
        wifibul_set_status("Scan worker failed to start");
    }
}

static void wifibul_poll_scan(void)
{
    if (!wifibul.scanning) return;

    bool ready = false;
    solar_os_wifi_ap_t tmp_results[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t tmp_count = 0U;
    esp_err_t tmp_err = ESP_OK;

    portENTER_CRITICAL(&wifibul_lock);
    if (wifibul.staging_ready) {
        ready = true;
        tmp_count = wifibul.staging_count;
        tmp_err = wifibul.staging_err;
        if (tmp_err == ESP_OK && tmp_count > 0U) {
            memcpy(tmp_results, wifibul.staging_results, sizeof(tmp_results));
        }
        wifibul.staging_ready = false;
    }
    portEXIT_CRITICAL(&wifibul_lock);

    if (!ready) return;

    wifibul.scanning = false;
    if (tmp_err == ESP_OK) {
        memcpy(wifibul.results, tmp_results, sizeof(wifibul.results));
        wifibul.result_count = tmp_count;
        if (wifibul.selected >= wifibul.result_count && wifibul.result_count > 0U) {
            wifibul.selected = wifibul.result_count - 1U;
        }

        /* Update radar target RSSI if active */
        if (wifibul.view == WIFIBUL_VIEW_RADAR) {
            bool found = false;
            for (size_t i = 0U; i < wifibul.result_count; i++) {
                if (strcmp(wifibul.results[i].ssid, wifibul.tracked_ssid) == 0) {
                    wifibul.tracked_rssi = wifibul.results[i].rssi;
                    memcpy(wifibul.tracked_bssid, wifibul.results[i].bssid, 6);
                    wifibul.tracked_found = true;
                    found = true;
                    break;
                }
            }
            if (!found) wifibul.tracked_found = false;
        }
        char msg[48];
        snprintf(msg, sizeof(msg), "Scan complete: %u networks found", (unsigned)wifibul.result_count);
        wifibul_set_status(msg);
    } else {
        wifibul_set_status("Wi-Fi scan failed");
    }
    wifibul.render_pending = true;
}

static int wifibul_quality_percent(int8_t rssi)
{
    if (rssi <= -95) return 0;
    if (rssi >= -45) return 100;
    return (int)((rssi + 95) * 2);
}

static void wifibul_play_beep(int quality)
{
    const uint32_t freq = 600U + (uint32_t)quality * 12U;
    const solar_os_audio_tone_step_t step = {
        .frequency_hz = freq,
        .duration_ms = 35U,
        .pause_ms = 0U,
    };
    const solar_os_audio_tone_request_t request = {
        .steps = &step,
        .step_count = 1U,
        .volume = 80U,
        .drop_if_busy = true,
    };
    uint32_t request_id = 0U;
    (void)solar_os_audio_tone_enqueue(&request, &request_id);
}

static void wifibul_radar_tick(void)
{
    if (!wifibul.tracked_found) return;

    const int quality = wifibul_quality_percent(wifibul.tracked_rssi);
    const uint32_t period_ms = 1200U - (uint32_t)((1200U - 120U) * (uint32_t)quality / 100U);

    if (wifibul.elapsed_ms >= wifibul.next_beep_ms) {
        wifibul.blink_on = !wifibul.blink_on;
        wifibul.next_beep_ms = wifibul.elapsed_ms + period_ms;
        if (wifibul.blink_on) {
            wifibul_play_beep(quality);
        }
        wifibul.render_pending = true;
    }
}

static void wifibul_set_status(const char *message)
{
    strncpy(wifibul.status_message, message, sizeof(wifibul.status_message) - 1U);
    wifibul.status_message[sizeof(wifibul.status_message) - 1U] = '\0';
    wifibul.status_until_ms = wifibul.elapsed_ms + 2500U;
    wifibul.render_pending = true;
}

/* ---------------------------------------------------------------------
 * UI Rendering
 * ------------------------------------------------------------------- */

static void wifibul_build_header(solar_os_appbar_header_t *out, char *status_buf, size_t status_buf_len)
{
    memset(out, 0, sizeof(*out));
    const char *view_name = wifibul.view == WIFIBUL_VIEW_LIST ? "Network List"
                            : wifibul.view == WIFIBUL_VIEW_DETAIL ? "AP Details"
                            : "Radar Tracker";
    out->title = view_name;
    out->show_back = true;
    snprintf(status_buf, status_buf_len, "%s", wifibul.scanning ? "Scanning..." : "");
    out->status_line = status_buf;
}

static void wifibul_draw_header(solar_os_gfx_t *gfx)
{
    char status_buf[32];
    solar_os_appbar_header_t header;
    wifibul_build_header(&header, status_buf, sizeof(status_buf));
    solar_os_appbar_draw_header(gfx, &header);
}

/* Builds the current footer's shortcut chips into a caller-owned buffer,
 * returning the count. Same set used by both drawing and click hit-testing
 * so they can never disagree about what's on screen. */
static size_t wifibul_build_footer_shortcuts(solar_os_appbar_shortcut_t *items, size_t max_items)
{
    size_t n = 0;
    if (wifibul.view != WIFIBUL_VIEW_DETAIL && n < max_items) {
        items[n].key = 'r'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Rescan");
        n++;
    }
    return n;
}

static void wifibul_draw_footer(solar_os_gfx_t *gfx, int width, int height)
{
    if (wifibul.status_until_ms > wifibul.elapsed_ms && wifibul.status_message[0] != '\0') {
        const int footer_h = solar_os_appbar_footer_height(gfx);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_rect(gfx, 0, height - footer_h, width, footer_h);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 8, height - footer_h / 4, wifibul.status_message);
        return;
    }

    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = wifibul_build_footer_shortcuts(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);
}

static void wifibul_draw_signal_bars(solar_os_gfx_t *gfx, int x, int y, int quality, bool inverted)
{
    const int bar_w = 4;
    const int gap = 2;
    const int max_h = 16;

    for (int i = 0; i < 4; i++) {
        const int bar_h = max_h * (i + 1) / 4;
        const int threshold = (i + 1) * 25;
        const bool active = quality >= threshold;

        if (inverted) {
            solar_os_gfx_set_color(gfx, active ? SOLAR_OS_GFX_COLOR_WHITE : SOLAR_OS_GFX_COLOR_BLACK);
        } else {
            solar_os_gfx_set_color(gfx, active ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_WHITE);
        }

        if (active) {
            solar_os_gfx_fill_rect(gfx, x + i * (bar_w + gap), y + (max_h - bar_h), bar_w, bar_h);
        } else {
            solar_os_gfx_rect(gfx, x + i * (bar_w + gap), y + (max_h - bar_h), bar_w, bar_h);
        }
    }
}

/* Shared geometry for the network list body: computed once, used by both
 * drawing and click hit-testing so they can never disagree about layout. */
typedef struct {
    int top;
    int row_h;
    int max_rows;
    size_t start;
} wifibul_list_layout_t;

static void wifibul_layout_list(int height, wifibul_list_layout_t *out)
{
    out->row_h = 24;
    out->top = WIFIBUL_HEADER_H + 4;
    out->max_rows = (height - WIFIBUL_FOOTER_H - out->top) / out->row_h;
    out->start = 0U;
    if (wifibul.result_count > (size_t)out->max_rows && wifibul.selected >= (size_t)out->max_rows) {
        out->start = wifibul.selected - (size_t)out->max_rows + 1U;
    }
}

/* Returns the result index under (x, y) in the network list body, or false
 * if the tap missed every row. */
static bool wifibul_hit_test_list(int width, int height, int16_t x, int16_t y, size_t *out_index)
{
    if (wifibul.result_count == 0U) return false;
    if (x < 4 || x >= width - 4) return false;

    wifibul_list_layout_t layout;
    wifibul_layout_list(height, &layout);
    if (y < layout.top) return false;

    const int row_in = (y - layout.top) / layout.row_h;
    if (row_in < 0 || row_in >= layout.max_rows) return false;

    const size_t idx = layout.start + (size_t)row_in;
    if (idx >= wifibul.result_count) return false;

    *out_index = idx;
    return true;
}

static void wifibul_draw_list(solar_os_gfx_t *gfx, int width, int height)
{
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    if (wifibul.scanning && wifibul.result_count == 0U) {
        solar_os_gfx_text(gfx, 14, WIFIBUL_HEADER_H + 30, "Scanning for Wi-Fi networks, please wait...");
        return;
    }
    if (wifibul.result_count == 0U) {
        solar_os_gfx_text(gfx, 14, WIFIBUL_HEADER_H + 30, "No networks found. Press [R] to rescan.");
        return;
    }

    wifibul_list_layout_t layout;
    wifibul_layout_list(height, &layout);
    const int row_h = layout.row_h;
    const int max_rows = layout.max_rows;
    const size_t start = layout.start;

    for (int row = 0; row < max_rows; row++) {
        const size_t idx = start + (size_t)row;
        if (idx >= wifibul.result_count) break;
        const int y = layout.top + row * row_h;
        const solar_os_wifi_ap_t *ap = &wifibul.results[idx];
        const bool is_sel = (idx == wifibul.selected);

        if (is_sel) {
            /* High-contrast inverted solid selection bar */
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 4, y, width - 8, row_h - 1);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }

        /* Signal 4-bar icon */
        wifibul_draw_signal_bars(gfx, 8, y + 3, wifibul_quality_percent(ap->rssi), is_sel);

        /* SSID */
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        const char *name = ap->ssid[0] != '\0' ? ap->ssid : "<Hidden SSID>";
        solar_os_gfx_text(gfx, 36, y + row_h - 7, name);

        /* MAC Address (BSSID) & Signal dBm & Channel */
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        char meta[48];
        snprintf(meta, sizeof(meta), "%02X:%02X:%02X:%02X:%02X:%02X  %d dBm  CH %u",
                 ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5],
                 (int)ap->rssi, (unsigned)ap->channel);
        const size_t meta_w = solar_os_gfx_text_width(gfx, meta);
        solar_os_gfx_text(gfx, width - (int)meta_w - 12, y + row_h - 7, meta);
    }
}

static void wifibul_draw_detail(solar_os_gfx_t *gfx, int width, int height)
{
    if (wifibul.selected >= wifibul.result_count) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 14, WIFIBUL_HEADER_H + 24, "No network selected");
        return;
    }
    const solar_os_wifi_ap_t *ap = &wifibul.results[wifibul.selected];
    const int quality = wifibul_quality_percent(ap->rssi);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    solar_os_gfx_text(gfx, 14, WIFIBUL_HEADER_H + 28, ap->ssid[0] != '\0' ? ap->ssid : "<Hidden SSID>");

    solar_os_gfx_line(gfx, 14, WIFIBUL_HEADER_H + 34, width - 14, WIFIBUL_HEADER_H + 34);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_16);
    int y = WIFIBUL_HEADER_H + 60;
    char line[64];

    char mac_str[24];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    snprintf(line, sizeof(line), "BSSID (MAC) : %s", mac_str);
    solar_os_gfx_text(gfx, 16, y, line);
    y += 24;

    snprintf(line, sizeof(line), "Security    : %s", ap->auth);
    solar_os_gfx_text(gfx, 16, y, line);
    y += 24;

    snprintf(line, sizeof(line), "Channel     : %u (2.4 GHz)", (unsigned)ap->channel);
    solar_os_gfx_text(gfx, 16, y, line);
    y += 24;

    snprintf(line, sizeof(line), "Signal RSSI : %d dBm (%d%% quality)", (int)ap->rssi, quality);
    solar_os_gfx_text(gfx, 16, y, line);
    y += 24;

    snprintf(line, sizeof(line), "Hidden SSID : %s", ap->hidden ? "Yes" : "No");
    solar_os_gfx_text(gfx, 16, y, line);
    y += 32;

    wifibul_draw_signal_bars(gfx, 16, y, quality, false);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 45, y + 14, "Press [Enter] to launch acoustic Radar Proximity Tracker");
    (void)height;
}

static void wifibul_draw_radar(solar_os_gfx_t *gfx, int width, int height)
{
    const int cx = width / 2;
    const int body_h = height - WIFIBUL_HEADER_H - WIFIBUL_FOOTER_H;
    const int cy = WIFIBUL_HEADER_H + body_h / 2;
    const int max_radius = (width < body_h ? width : body_h) / 2 - 18;

    /* High contrast concentric circles */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    for (int r = max_radius / 3; r <= max_radius; r += max_radius / 3) {
        solar_os_gfx_circle(gfx, cx, cy, r);
    }
    solar_os_gfx_line(gfx, cx - max_radius, cy, cx + max_radius, cy);
    solar_os_gfx_line(gfx, cx, cy - max_radius, cx, cy + max_radius);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    const size_t name_w = solar_os_gfx_text_width(gfx, wifibul.tracked_ssid);
    solar_os_gfx_text(gfx, cx - (int)name_w / 2, WIFIBUL_HEADER_H + 24, wifibul.tracked_ssid);

    if (!wifibul.tracked_found) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, cx - 80, cy, "Network signal lost...");
        return;
    }

    const int quality = wifibul_quality_percent(wifibul.tracked_rssi);
    const int blip_radius = 6 + (int)((float)max_radius * (100 - quality) / 100.0f);

    if (wifibul.blink_on) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_circle(gfx, cx, cy, 6);
        solar_os_gfx_circle(gfx, cx, cy, blip_radius > max_radius ? max_radius : blip_radius);
    }

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    char big[32];
    snprintf(big, sizeof(big), "%d%% Quality", quality);
    const size_t bw = solar_os_gfx_text_width(gfx, big);
    solar_os_gfx_text(gfx, cx - (int)bw / 2, cy + max_radius + 20, big);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char dbm[32];
    snprintf(dbm, sizeof(dbm), "%d dBm  (Closer = Higher Beep Pitch)", (int)wifibul.tracked_rssi);
    const size_t dbm_w = solar_os_gfx_text_width(gfx, dbm);
    solar_os_gfx_text(gfx, cx - (int)dbm_w / 2, cy + max_radius + 36, dbm);
}

static void wifibul_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    wifibul_draw_header(gfx);

    switch (wifibul.view) {
    case WIFIBUL_VIEW_DETAIL:
        wifibul_draw_detail(gfx, width, height);
        break;
    case WIFIBUL_VIEW_RADAR:
        wifibul_draw_radar(gfx, width, height);
        break;
    case WIFIBUL_VIEW_LIST:
    default:
        wifibul_draw_list(gfx, width, height);
        break;
    }

    wifibul_draw_footer(gfx, width, height);
    solar_os_gfx_present(gfx);
    wifibul.render_pending = false;
}

/* ---------------------------------------------------------------------
 * Events and Lifecycle
 * ------------------------------------------------------------------- */

/* Drill-down back navigation: RADAR -> DETAIL -> LIST -> exit app.
 * Shared by the keyboard Backspace/Left path and the header back button so
 * they can never disagree about what "back" means in each view. */
static void wifibul_go_back(solar_os_context_t *ctx)
{
    if (wifibul.view == WIFIBUL_VIEW_RADAR) {
        wifibul.view = WIFIBUL_VIEW_DETAIL;
        wifibul.render_pending = true;
    } else if (wifibul.view == WIFIBUL_VIEW_DETAIL) {
        wifibul.view = WIFIBUL_VIEW_LIST;
        wifibul.render_pending = true;
    } else {
        solar_os_context_request_exit(ctx);
    }
}

static void wifibul_handle_char(solar_os_context_t *ctx, char ch)
{
    const unsigned char uch = (unsigned char)ch;

    if (uch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return;
    }

    if (ch == 'r' || ch == 'R') {
        wifibul_start_scan();
        wifibul_set_status("Scanning Wi-Fi...");
        return;
    }

    if (wifibul.view == WIFIBUL_VIEW_LIST) {
        if (uch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
            if (wifibul.selected > 0U) {
                wifibul.selected--;
                wifibul.render_pending = true;
            }
            return;
        }
        if (uch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
            if (wifibul.selected + 1U < wifibul.result_count) {
                wifibul.selected++;
                wifibul.render_pending = true;
            }
            return;
        }
        if (ch == '\n' || ch == '\r') {
            if (wifibul.result_count > 0U && wifibul.selected < wifibul.result_count) {
                wifibul.view = WIFIBUL_VIEW_DETAIL;
                wifibul.render_pending = true;
            }
            return;
        }
        return;
    }

    if (wifibul.view == WIFIBUL_VIEW_DETAIL) {
        if (uch == 0x08U || uch == 0x7fU || uch == SOLAR_OS_KEY_LEFT) {
            wifibul_go_back(ctx);
            return;
        }
        if (ch == '\n' || ch == '\r') {
            if (wifibul.selected < wifibul.result_count) {
                const solar_os_wifi_ap_t *ap = &wifibul.results[wifibul.selected];
                strlcpy(wifibul.tracked_ssid, ap->ssid, sizeof(wifibul.tracked_ssid));
                memcpy(wifibul.tracked_bssid, ap->bssid, 6);
                wifibul.tracked_rssi = ap->rssi;
                wifibul.tracked_found = true;
                wifibul.view = WIFIBUL_VIEW_RADAR;
                wifibul.next_rescan_ms = wifibul.elapsed_ms + WIFIBUL_RESCAN_MS;
                wifibul.next_beep_ms = wifibul.elapsed_ms;
                wifibul.blink_on = false;
                wifibul.render_pending = true;
            }
            return;
        }
        return;
    }

    if (wifibul.view == WIFIBUL_VIEW_RADAR) {
        if (uch == 0x08U || uch == 0x7fU || uch == SOLAR_OS_KEY_LEFT) {
            wifibul_go_back(ctx);
            return;
        }
    }
}

static bool wifibul_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    switch (event->type) {
    case SOLAR_OS_EVENT_CHAR:
        wifibul_handle_char(ctx, event->data.ch);
        break;
    case SOLAR_OS_EVENT_TICK:
        wifibul.elapsed_ms += WIFIBUL_TICK_MS;
        wifibul_poll_scan();
        if (wifibul.view == WIFIBUL_VIEW_RADAR) {
            wifibul_radar_tick();
            if (wifibul.elapsed_ms >= wifibul.next_rescan_ms) {
                wifibul.next_rescan_ms = wifibul.elapsed_ms + WIFIBUL_RESCAN_MS;
                wifibul_start_scan();
            }
        }
        if (wifibul.status_until_ms != 0U && wifibul.status_until_ms <= wifibul.elapsed_ms &&
            wifibul.status_until_ms + WIFIBUL_TICK_MS > wifibul.elapsed_ms) {
            wifibul.render_pending = true;
        }
        if (wifibul.render_pending) {
            wifibul_render(ctx);
        }
        break;
    case SOLAR_OS_EVENT_RESUME:
        wifibul.render_pending = true;
        wifibul_render(ctx);
        break;

    case SOLAR_OS_EVENT_SCROLL:
        if (wifibul.view == WIFIBUL_VIEW_LIST) {
            const bool down = event->data.scroll.delta < 0;
            if (down) {
                if (wifibul.selected + 1U < wifibul.result_count) {
                    wifibul.selected++;
                    wifibul.render_pending = true;
                }
            } else if (wifibul.selected > 0U) {
                wifibul.selected--;
                wifibul.render_pending = true;
            }
            if (wifibul.render_pending) wifibul_render(ctx);
        }
        break;

    case SOLAR_OS_EVENT_CLICK: {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) break;

        char status_buf[32];
        solar_os_appbar_header_t header;
        wifibul_build_header(&header, status_buf, sizeof(status_buf));

        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, event->data.click.x, event->data.click.y, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                wifibul_go_back(ctx);
            }
            if (wifibul.render_pending) wifibul_render(ctx);
            break;
        }

        if (wifibul.view == WIFIBUL_VIEW_LIST) {
            const int width = (int)solar_os_gfx_width(gfx);
            const int height = (int)solar_os_gfx_height(gfx);
            size_t idx;
            if (wifibul_hit_test_list(width, height, event->data.click.x, event->data.click.y, &idx)) {
                wifibul.selected = idx;
                wifibul.view = WIFIBUL_VIEW_DETAIL;
                wifibul.render_pending = true;
                wifibul_render(ctx);
                break;
            }
        }

        const bool showing_status = wifibul.status_until_ms > wifibul.elapsed_ms && wifibul.status_message[0] != '\0';
        if (!showing_status) {
            solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
            const size_t count = wifibul_build_footer_shortcuts(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
            const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };

            solar_os_appbar_hit_t fhit;
            if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, event->data.click.x, event->data.click.y, &fhit) &&
                fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM) {
                wifibul_handle_char(ctx, items[fhit.index].key);
                if (wifibul.render_pending) wifibul_render(ctx);
            }
        }
        break;
    }

    default:
        break;
    }
    return true;
}

static esp_err_t wifibul_start(solar_os_context_t *ctx)
{
    memset(&wifibul, 0, sizeof(wifibul));
    wifibul.view = WIFIBUL_VIEW_LIST;
    wifibul.render_pending = true;

    solar_os_context_set_graphics_active(ctx, true);
    wifibul_start_scan();
    wifibul_render(ctx);
    return ESP_OK;
}

static void wifibul_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (wifibul.task != NULL) {
        solar_os_task_delete(wifibul.task);
        wifibul.task = NULL;
    }
}

static void wifibul_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    snprintf(buffer, buffer_len, "WiFi: %u APs", (unsigned)wifibul.result_count);
}

const solar_os_app_t solar_os_wifibul_app = {
    .name = "wifibul",
    .summary = "Wi-Fi network finder, radar and signal analyzer",
    .flags = 0,
    .start = wifibul_start,
    .stop = wifibul_stop,
    .event = wifibul_event,
    .title = wifibul_title,
    .state_slot = &wifibul_state_ptr,
    .state_size = sizeof(wifibul_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = WIFIBUL_TASK_STACK,
    .worker_stack_external = false,
    .tick_interval_ms = WIFIBUL_TICK_MS,
    .tick_deadline_ms = WIFIBUL_TICK_MS * 3U,
};
