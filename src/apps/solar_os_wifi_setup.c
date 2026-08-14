#include "solar_os_wifi_setup.h"

#include <ctype.h>
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
#include "solar_os_wifi.h"

#define WIFI_SETUP_MAX_APS 16
#define WIFI_SETUP_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(WIFI_SETUP_STACK_SIZE);

typedef enum {
    WIFI_VIEW_SCAN_LIST = 0,
    WIFI_VIEW_PASSWORD_INPUT = 1,
    WIFI_VIEW_CONNECTING = 2,
} wifi_setup_view_t;

typedef struct {
    wifi_setup_view_t view;
    solar_os_wifi_ap_t aps[WIFI_SETUP_MAX_APS];
    size_t ap_count;
    size_t selected_ap;
    char target_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    char password[SOLAR_OS_WIFI_PASSWORD_MAX + 1];
    size_t password_len;
    bool show_password;
    char status_msg[64];
    uint32_t status_timer_ms;
    uint32_t connect_start_ms;
} wifi_setup_state_t;

static void *wifi_setup_state_ptr;
#define wsetup (*(wifi_setup_state_t *)wifi_setup_state_ptr)

static void wifi_do_scan(void)
{
    wsetup.ap_count = 0;
    strlcpy(wsetup.status_msg, "Scanning for 2.4GHz Wi-Fi networks...", sizeof(wsetup.status_msg));

    (void)solar_os_wifi_start();
    size_t found = 0;
    esp_err_t err = solar_os_wifi_scan(wsetup.aps, WIFI_SETUP_MAX_APS, &found);
    if (err == ESP_OK && found > 0) {
        wsetup.ap_count = found;
        snprintf(wsetup.status_msg, sizeof(wsetup.status_msg), "Found %u networks. Select and press ENTER.", (unsigned)found);
    } else {
        strlcpy(wsetup.status_msg, "No networks found. Press 'R' to rescan.", sizeof(wsetup.status_msg));
    }
}

static void wifi_setup_render(solar_os_context_t *ctx)
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
    solar_os_gfx_text(gfx, 8, 16, "WI-FI MANAGER & SETUP");

    solar_os_wifi_status_t wst;
    solar_os_wifi_get_status(&wst);
    char ip_hdr[32] = "OFFLINE";
    if (wst.connected && wst.has_ip) {
        snprintf(ip_hdr, sizeof(ip_hdr), "IP: %s", wst.ip);
    }
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t ih_w = solar_os_gfx_text_width(gfx, ip_hdr);
    solar_os_gfx_text(gfx, screen_w - (int)ih_w - 8, 16, ip_hdr);

    /* 2. Main Area: Network List */
    if (wsetup.view == WIFI_VIEW_SCAN_LIST) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 12, 42, "AVAILABLE WI-FI NETWORKS");
        solar_os_gfx_line(gfx, 10, 46, screen_w - 10, 46);

        const int list_top_y = 52;
        const int row_h = 24;
        const size_t max_visible = 8;

        if (wsetup.ap_count == 0) {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 24, 100, "(No networks in list - Scanning...)");
            solar_os_gfx_text(gfx, 24, 130, "Press 'R' to start network scan.");
        } else {
            for (size_t i = 0; i < max_visible && i < wsetup.ap_count; i++) {
                const int ry = list_top_y + (int)i * row_h;
                const bool is_sel = (i == wsetup.selected_ap);
                const solar_os_wifi_ap_t *ap = &wsetup.aps[i];

                if (is_sel) {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_fill_rect(gfx, 12, ry, screen_w - 24, row_h - 2);
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
                } else {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                }

                /* Lock / Open Badge */
                const bool is_secured = (strstr(ap->auth, "OPEN") == NULL && ap->auth[0] != '\0');
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                solar_os_gfx_text(gfx, 18, ry + 16, is_secured ? "[SEC]" : "[OPEN]");

                /* SSID */
                solar_os_gfx_set_font(gfx, is_sel ? SOLAR_OS_GFX_FONT_BOLD : SOLAR_OS_GFX_FONT_SMALL);
                solar_os_gfx_text(gfx, 75, ry + 16, ap->ssid[0] ? ap->ssid : "<Hidden Network>");

                /* Signal Bars / dBm */
                char sig_str[32];
                snprintf(sig_str, sizeof(sig_str), "%d dBm", ap->rssi);
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                solar_os_gfx_text(gfx, screen_w - 85, ry + 16, sig_str);
            }
        }
    }

    /* 3. Password Input Modal Box */
    if (wsetup.view == WIFI_VIEW_PASSWORD_INPUT) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, 30, 60, screen_w - 60, 160);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, 30, 60, screen_w - 60, 160);
        solar_os_gfx_rect(gfx, 32, 62, screen_w - 64, 156);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 45, 85, "ENTER WI-FI PASSWORD");

        char ssid_prompt[64];
        snprintf(ssid_prompt, sizeof(ssid_prompt), "Network: %s", wsetup.target_ssid);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 45, 110, ssid_prompt);

        /* Input Box */
        solar_os_gfx_rect(gfx, 45, 125, screen_w - 90, 30);

        char pass_display[SOLAR_OS_WIFI_PASSWORD_MAX + 4];
        if (wsetup.show_password) {
            strlcpy(pass_display, wsetup.password, sizeof(pass_display));
        } else {
            memset(pass_display, '*', wsetup.password_len);
            pass_display[wsetup.password_len] = '\0';
        }
        strlcat(pass_display, "_", sizeof(pass_display));

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 55, 146, pass_display);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 45, 185, "[ENTER] Connect | [TAB] Show/Hide | [ESC] Cancel");
    }

    /* 4. Connecting Overlay */
    if (wsetup.view == WIFI_VIEW_CONNECTING) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, 40, 80, screen_w - 80, 120);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, 40, 80, screen_w - 80, 120);
        solar_os_gfx_rect(gfx, 42, 82, screen_w - 84, 116);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 70, 115, "CONNECTING TO NETWORK...");

        char c_target[64];
        snprintf(c_target, sizeof(c_target), "%s", wsetup.target_ssid);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 70, 145, c_target);
        solar_os_gfx_text(gfx, 70, 170, "Please wait, obtaining IP address...");
    }

    /* 5. Status / Message Line (Y: 254) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 12, 268, wsetup.status_msg);

    /* 6. Footer Navigation Bar */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 293, "[UP/DOWN] Select | [ENTER] Connect | [R] Rescan | [ESC] Exit");

    solar_os_gfx_present(gfx);
}

static esp_err_t wifi_setup_start(solar_os_context_t *ctx)
{
    wsetup.view = WIFI_VIEW_SCAN_LIST;
    wsetup.ap_count = 0;
    wsetup.selected_ap = 0;
    wsetup.password[0] = '\0';
    wsetup.password_len = 0;
    wsetup.show_password = false;
    wsetup.status_msg[0] = '\0';

    solar_os_context_set_graphics_active(ctx, true);
    wifi_do_scan();
    wifi_setup_render(ctx);
    return ESP_OK;
}

static void wifi_setup_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool wifi_setup_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (wsetup.view == WIFI_VIEW_CONNECTING) {
            solar_os_wifi_status_t wst;
            solar_os_wifi_get_status(&wst);
            if (wst.connected && wst.has_ip) {
                wsetup.view = WIFI_VIEW_SCAN_LIST;
                snprintf(wsetup.status_msg, sizeof(wsetup.status_msg), "Connected to %s! IP: %s", wsetup.target_ssid, wst.ip);
                wifi_setup_render(ctx);
            } else {
                const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
                if (now - wsetup.connect_start_ms >= 12000U) {
                    wsetup.view = WIFI_VIEW_SCAN_LIST;
                    snprintf(wsetup.status_msg, sizeof(wsetup.status_msg), "Connection timeout. Check password.");
                    wifi_setup_render(ctx);
                }
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (wsetup.view == WIFI_VIEW_SCAN_LIST) {
            if (ch == SOLAR_OS_KEY_UP || ch == 'k' || ch == 'K' || ch == 'w' || ch == 'W') {
                if (wsetup.selected_ap > 0) {
                    wsetup.selected_ap--;
                    wifi_setup_render(ctx);
                }
                return true;
            }
            if (ch == SOLAR_OS_KEY_DOWN || ch == 'j' || ch == 'J' || ch == 's' || ch == 'S') {
                if (wsetup.ap_count > 0 && wsetup.selected_ap + 1 < wsetup.ap_count) {
                    wsetup.selected_ap++;
                    wifi_setup_render(ctx);
                }
                return true;
            }
            if (ch == '\r' || ch == '\n') {
                if (wsetup.ap_count > 0 && wsetup.selected_ap < wsetup.ap_count) {
                    strlcpy(wsetup.target_ssid, wsetup.aps[wsetup.selected_ap].ssid, sizeof(wsetup.target_ssid));
                    wsetup.password[0] = '\0';
                    wsetup.password_len = 0;
                    wsetup.view = WIFI_VIEW_PASSWORD_INPUT;
                    wifi_setup_render(ctx);
                }
                return true;
            }
            if (ch == 'r' || ch == 'R') {
                wifi_do_scan();
                wifi_setup_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
                solar_os_context_request_exit(ctx);
                return true;
            }
        } else if (wsetup.view == WIFI_VIEW_PASSWORD_INPUT) {
            if (ch == '\r' || ch == '\n') {
                wsetup.view = WIFI_VIEW_CONNECTING;
                wsetup.connect_start_ms = (uint32_t)(esp_timer_get_time() / 1000U);
                (void)solar_os_wifi_connect(wsetup.target_ssid, wsetup.password);
                wifi_setup_render(ctx);
                return true;
            }
            if (ch == '\t') {
                wsetup.show_password = !wsetup.show_password;
                wifi_setup_render(ctx);
                return true;
            }
            if (ch == '\b' || ch == 127) {
                if (wsetup.password_len > 0) {
                    wsetup.password_len--;
                    wsetup.password[wsetup.password_len] = '\0';
                    wifi_setup_render(ctx);
                }
                return true;
            }
            if (ch == SOLAR_OS_KEY_ESCAPE) {
                wsetup.view = WIFI_VIEW_SCAN_LIST;
                wifi_setup_render(ctx);
                return true;
            }
            if (isprint((int)ch) && wsetup.password_len < SOLAR_OS_WIFI_PASSWORD_MAX) {
                wsetup.password[wsetup.password_len++] = ch;
                wsetup.password[wsetup.password_len] = '\0';
                wifi_setup_render(ctx);
                return true;
            }
        }
    }

    return false;
}

const solar_os_app_t solar_os_wifi_setup_app = {
    .name = "wifi_setup",
    .summary = "graphical Wi-Fi network scanner and connection tool",
    .flags = 0,
    .start = wifi_setup_start,
    .stop = wifi_setup_stop,
    .event = wifi_setup_event,
    .state_slot = &wifi_setup_state_ptr,
    .state_size = sizeof(wifi_setup_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = WIFI_SETUP_STACK_SIZE,
};
