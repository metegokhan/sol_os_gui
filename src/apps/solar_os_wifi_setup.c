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
#include "solar_os_appbar.h"
#include "solar_os_help.h"

#define WIFI_SETUP_MAX_APS 16
#define WIFI_SETUP_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(WIFI_SETUP_STACK_SIZE);

#define WIFI_TAB_H 22
#define WIFI_TAB_Y 26

#define WIFI_SCAN_LIST_TOP 54
#define WIFI_SCAN_ROW_H 24
#define WIFI_SCAN_MAX_VISIBLE 8

#define WIFI_SAVED_LIST_TOP 54
#define WIFI_SAVED_ROW_H 34
#define WIFI_SAVED_MAX_VISIBLE 5

typedef enum {
    WIFI_TAB_SCAN = 0,
    WIFI_TAB_SAVED = 1,
} wifi_tab_t;

typedef enum {
    WIFI_VIEW_NORMAL = 0,
    WIFI_VIEW_PASSWORD_INPUT = 1,
    WIFI_VIEW_CONNECTING = 2,
} wifi_setup_view_t;

typedef struct {
    wifi_tab_t tab;
    wifi_setup_view_t view;

    /* Scan tab state */
    solar_os_wifi_ap_t aps[WIFI_SETUP_MAX_APS];
    size_t ap_count;
    size_t selected_ap;
    char target_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    char password[SOLAR_OS_WIFI_PASSWORD_MAX + 1];
    size_t password_len;
    bool show_password;

    /* Saved tab state */
    solar_os_wifi_saved_entry_t saved[SOLAR_OS_WIFI_SAVED_MAX];
    size_t saved_count;
    size_t selected_saved;

    char status_msg[64];
    uint32_t connect_start_ms;
    bool show_help;
} wifi_setup_state_t;

static void *wifi_setup_state_ptr;
#define wsetup (*(wifi_setup_state_t *)wifi_setup_state_ptr)

static const char *const wifi_help_lines[] = {
    "Wi-Fi Connection & Profile Manager",
    "",
    "  [Tab 1: Scan & Join]",
    "  - Select nearby network, press Enter / tap to join.",
    "  - Type password, Tab shows/hides, Enter connects.",
    "",
    "  [Tab 2: Saved Networks]",
    "  - [U] / [D]: Move priority Up / Down.",
    "  - [T]: Toggle Active [ON] / Inactive [OFF].",
    "  - [Enter]: Connect to selected network now.",
    "  - [X]: Delete selected network.",
    "  - [E]: Export to SD / [I]: Import from SD.",
    "  - [TAB]: Switch between Scan and Saved tabs.",
};
#define WIFI_HELP_LINE_COUNT (sizeof(wifi_help_lines) / sizeof(wifi_help_lines[0]))

static void wifi_refresh_saved_list(void)
{
    wsetup.saved_count = 0;
    (void)solar_os_wifi_get_saved_entries(wsetup.saved, SOLAR_OS_WIFI_SAVED_MAX, &wsetup.saved_count);
    if (wsetup.selected_saved >= wsetup.saved_count && wsetup.saved_count > 0) {
        wsetup.selected_saved = wsetup.saved_count - 1;
    }
}

static size_t wifi_build_footer(solar_os_appbar_shortcut_t *items, size_t max)
{
    size_t n = 0;
    if (wsetup.tab == WIFI_TAB_SCAN) {
        if (n < max) { items[n].key = 'r'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Rescan"); n++; }
        if (n < max) { items[n].key = '\t'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Saved [TAB]"); n++; }
    } else {
        if (n < max) { items[n].key = 'u'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Up"); n++; }
        if (n < max) { items[n].key = 'd'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Down"); n++; }
        if (n < max) { items[n].key = 't'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Toggle"); n++; }
        if (n < max) { items[n].key = 'x'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Delete"); n++; }
        if (n < max) { items[n].key = 'e'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Exp SD"); n++; }
        if (n < max) { items[n].key = 'i'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Imp SD"); n++; }
        if (n < max) { items[n].key = '\t'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Scan [TAB]"); n++; }
    }
    if (n < max) { solar_os_help_chip(&items[n]); n++; }
    return n;
}

static void wifi_do_scan(void)
{
    wsetup.ap_count = 0;
    strlcpy(wsetup.status_msg, "Scanning for 2.4 GHz networks...", sizeof(wsetup.status_msg));

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

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header with IP / connection state */
    solar_os_wifi_status_t wst;
    solar_os_wifi_get_status(&wst);
    char status_line[48] = "Offline";
    if (wst.connected && wst.has_ip) {
        snprintf(status_line, sizeof(status_line), "IP: %s", wst.ip);
    } else if (wst.state == SOLAR_OS_WIFI_STATE_CONNECTING) {
        snprintf(status_line, sizeof(status_line), "Connecting...");
    }
    solar_os_appbar_header_t header = {0};
    header.title = "Wi-Fi Setup";
    header.show_back = true;
    header.status_line = status_line;
    solar_os_appbar_draw_header(gfx, &header);

    /* 2. Dual Tab Bar */
    const int tab_w = (screen_w - 20) / 2;
    const int tab_y = WIFI_TAB_Y;

    /* Tab 0: Scan & Join */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    if (wsetup.tab == WIFI_TAB_SCAN) {
        solar_os_gfx_fill_rect(gfx, 10, tab_y, tab_w, WIFI_TAB_H);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    } else {
        solar_os_gfx_rect(gfx, 10, tab_y, tab_w, WIFI_TAB_H);
    }
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 18, tab_y + 15, "[1] Scan & Join");

    /* Tab 1: Saved Networks */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    if (wsetup.tab == WIFI_TAB_SAVED) {
        solar_os_gfx_fill_rect(gfx, 10 + tab_w + 4, tab_y, tab_w - 4, WIFI_TAB_H);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    } else {
        solar_os_gfx_rect(gfx, 10 + tab_w + 4, tab_y, tab_w - 4, WIFI_TAB_H);
    }
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 18 + tab_w + 4, tab_y + 15, "[2] Saved Networks");

    /* 3. Main Area: Tab 0 (Scan & Join) */
    if (wsetup.tab == WIFI_TAB_SCAN && wsetup.view == WIFI_VIEW_NORMAL) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_line(gfx, 10, 50, screen_w - 10, 50);

        const int list_top_y = WIFI_SCAN_LIST_TOP;
        const int row_h = WIFI_SCAN_ROW_H;
        const size_t max_visible = WIFI_SCAN_MAX_VISIBLE;

        if (wsetup.ap_count == 0) {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 24, 90, "(No networks in list - Scanning...)");
            solar_os_gfx_text(gfx, 24, 115, "Press 'R' to scan for Wi-Fi networks.");
        } else {
            size_t scroll_offset = 0;
            if (wsetup.selected_ap >= max_visible) {
                scroll_offset = wsetup.selected_ap - max_visible + 1;
            }

            for (size_t row = 0; row < max_visible && (scroll_offset + row) < wsetup.ap_count; row++) {
                const size_t idx = scroll_offset + row;
                const int ry = list_top_y + (int)row * row_h;
                const bool is_sel = (idx == wsetup.selected_ap);
                const solar_os_wifi_ap_t *ap = &wsetup.aps[idx];

                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

                if (is_sel) {
                    solar_os_gfx_rect(gfx, 8, ry, screen_w - 16, row_h - 2);
                    solar_os_gfx_rect(gfx, 9, ry + 1, screen_w - 18, row_h - 4);
                    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
                    solar_os_gfx_text(gfx, 12, ry + 16, ">");
                }

                const bool is_open = (strstr(ap->auth, "open") != NULL || strstr(ap->auth, "OPEN") != NULL || ap->auth[0] == '\0');
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                solar_os_gfx_text(gfx, 24, ry + 16, is_open ? "[OPEN]" : "[SEC]");

                solar_os_gfx_set_font(gfx, is_sel ? SOLAR_OS_GFX_FONT_BOLD : SOLAR_OS_GFX_FONT_SMALL);
                const char *name = ap->ssid[0] ? ap->ssid : "<Hidden Network>";
                solar_os_gfx_text(gfx, 76, ry + 16, name);

                char sig_str[32];
                snprintf(sig_str, sizeof(sig_str), "%d dBm", ap->rssi);
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                solar_os_gfx_text(gfx, screen_w - 78, ry + 16, sig_str);
            }
        }
    }

    /* 4. Main Area: Tab 1 (Saved Networks) */
    if (wsetup.tab == WIFI_TAB_SAVED && wsetup.view == WIFI_VIEW_NORMAL) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_line(gfx, 10, 50, screen_w - 10, 50);

        const int list_top_y = WIFI_SAVED_LIST_TOP;
        const int row_h = WIFI_SAVED_ROW_H;
        const size_t max_visible = WIFI_SAVED_MAX_VISIBLE;

        if (wsetup.saved_count == 0) {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 24, 90, "(No saved networks found in NVS or SD card)");
            solar_os_gfx_text(gfx, 24, 115, "Connect to a network from the Scan tab, or add to SD card saved.wifi.");
        } else {
            size_t scroll_offset = 0;
            if (wsetup.selected_saved >= max_visible) {
                scroll_offset = wsetup.selected_saved - max_visible + 1;
            }

            for (size_t row = 0; row < max_visible && (scroll_offset + row) < wsetup.saved_count; row++) {
                const size_t idx = scroll_offset + row;
                const int ry = list_top_y + (int)row * row_h;
                const bool is_sel = (idx == wsetup.selected_saved);
                const solar_os_wifi_saved_entry_t *entry = &wsetup.saved[idx];

                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

                if (is_sel) {
                    solar_os_gfx_rect(gfx, 8, ry, screen_w - 16, row_h - 2);
                    solar_os_gfx_rect(gfx, 9, ry + 1, screen_w - 18, row_h - 4);
                    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
                    solar_os_gfx_text(gfx, 12, ry + 14, ">");
                }

                /* Source & Status Badges: [NV] / [SD] and [ON] / [OFF] and #Priority */
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                char badges[32];
                snprintf(badges, sizeof(badges), "%s %s #%u",
                         entry->is_sd ? "[SD]" : "[NV]",
                         entry->enabled ? "[ON]" : "[OFF]",
                         (unsigned)(idx + 1));
                solar_os_gfx_text(gfx, 24, ry + 14, badges);

                /* Line 1: SSID */
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
                solar_os_gfx_text(gfx, 115, ry + 14, entry->ssid);

                /* Line 2: Password */
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                char pass_line[80];
                if (entry->password[0] != '\0') {
                    snprintf(pass_line, sizeof(pass_line), "Pass: %s", entry->password);
                } else {
                    snprintf(pass_line, sizeof(pass_line), "Pass: <Open / None>");
                }
                solar_os_gfx_text(gfx, 24, ry + 28, pass_line);
            }
        }
    }

    /* 5. Password Input Modal Box */
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

    /* 6. Connecting Overlay */
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

    /* 7. Status Message Line */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 12, 268, wsetup.status_msg);

    /* 8. Shared Footer Chips */
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = wifi_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);

    if (wsetup.show_help) {
        solar_os_help_draw(gfx, "Wi-Fi Setup - Help", wifi_help_lines, WIFI_HELP_LINE_COUNT);
    }

    solar_os_gfx_present(gfx);
}

static esp_err_t wifi_setup_start(solar_os_context_t *ctx)
{
    wsetup.tab = WIFI_TAB_SCAN;
    wsetup.view = WIFI_VIEW_NORMAL;
    wsetup.ap_count = 0;
    wsetup.selected_ap = 0;
    wsetup.selected_saved = 0;
    wsetup.password[0] = '\0';
    wsetup.password_len = 0;
    wsetup.show_password = false;
    wsetup.status_msg[0] = '\0';

    solar_os_context_set_graphics_active(ctx, true);
    wifi_refresh_saved_list();
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
                wsetup.view = WIFI_VIEW_NORMAL;
                snprintf(wsetup.status_msg, sizeof(wsetup.status_msg), "Connected to %s! IP: %s", wsetup.target_ssid, wst.ip);
                wifi_refresh_saved_list();
                wifi_setup_render(ctx);
            } else {
                const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
                if (now - wsetup.connect_start_ms >= 12000U) {
                    wsetup.view = WIFI_VIEW_NORMAL;
                    snprintf(wsetup.status_msg, sizeof(wsetup.status_msg), "Connection timeout. Check password.");
                    wifi_setup_render(ctx);
                }
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;
        const int screen_w = (int)solar_os_gfx_width(gfx);

        if (wsetup.show_help) {
            wsetup.show_help = false;
            wifi_setup_render(ctx);
            return true;
        }

        /* 1. Header Back Arrow */
        solar_os_appbar_header_t header = {0};
        header.show_back = true;
        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, px, py, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                if (wsetup.view == WIFI_VIEW_PASSWORD_INPUT) {
                    wsetup.view = WIFI_VIEW_NORMAL;
                    wifi_setup_render(ctx);
                } else {
                    solar_os_context_request_exit(ctx);
                }
            }
            return true;
        }

        /* 2. Tab Bar Clicks (Y: 26..48) */
        if (py >= WIFI_TAB_Y && py < WIFI_TAB_Y + WIFI_TAB_H && wsetup.view == WIFI_VIEW_NORMAL) {
            const int tab_w = (screen_w - 20) / 2;
            if (px >= 10 && px < 10 + tab_w) {
                wsetup.tab = WIFI_TAB_SCAN;
                wifi_setup_render(ctx);
                return true;
            } else if (px >= 10 + tab_w + 4 && px < screen_w - 10) {
                wsetup.tab = WIFI_TAB_SAVED;
                wifi_refresh_saved_list();
                wifi_setup_render(ctx);
                return true;
            }
        }

        /* 3. Footer Clicks */
        solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
        const size_t count = wifi_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < count) {
                const char key = items[fhit.index].key;
                if (key == 'r') {
                    wifi_do_scan();
                } else if (key == '\t') {
                    wsetup.tab = (wsetup.tab == WIFI_TAB_SCAN) ? WIFI_TAB_SAVED : WIFI_TAB_SCAN;
                    if (wsetup.tab == WIFI_TAB_SAVED) wifi_refresh_saved_list();
                } else if (key == 'u') {
                    if (wsetup.selected_saved > 0 && wsetup.selected_saved < wsetup.saved_count) {
                        (void)solar_os_wifi_move_profile(wsetup.selected_saved, wsetup.selected_saved - 1);
                        wsetup.selected_saved--;
                        wifi_refresh_saved_list();
                    }
                } else if (key == 'd') {
                    if (wsetup.selected_saved + 1 < wsetup.saved_count) {
                        (void)solar_os_wifi_move_profile(wsetup.selected_saved, wsetup.selected_saved + 1);
                        wsetup.selected_saved++;
                        wifi_refresh_saved_list();
                    }
                } else if (key == 't') {
                    if (wsetup.selected_saved < wsetup.saved_count) {
                        (void)solar_os_wifi_toggle_profile(wsetup.saved[wsetup.selected_saved].ssid);
                        wifi_refresh_saved_list();
                    }
                } else if (key == 'x') {
                    if (wsetup.selected_saved < wsetup.saved_count) {
                        const solar_os_wifi_saved_entry_t *entry = &wsetup.saved[wsetup.selected_saved];
                        if (entry->is_sd) {
                            (void)solar_os_wifi_delete_sd_entry(entry->ssid);
                        } else {
                            (void)solar_os_wifi_forget_ssid(entry->ssid);
                        }
                        wifi_refresh_saved_list();
                    }
                } else if (key == 'e') {
                    (void)solar_os_wifi_save_to_sd_file();
                    snprintf(wsetup.status_msg, sizeof(wsetup.status_msg), "Exported saved profiles to SD card.");
                    wifi_refresh_saved_list();
                } else if (key == 'i') {
                    (void)solar_os_wifi_sync_sd_file();
                    snprintf(wsetup.status_msg, sizeof(wsetup.status_msg), "Imported profiles from SD card.");
                    wifi_refresh_saved_list();
                } else {
                    wsetup.show_help = true;
                }
                wifi_setup_render(ctx);
            }
            return true;
        }

        /* 4. Tap Row in Scan Tab */
        if (wsetup.tab == WIFI_TAB_SCAN && wsetup.view == WIFI_VIEW_NORMAL && wsetup.ap_count > 0) {
            if (py >= WIFI_SCAN_LIST_TOP && py < WIFI_SCAN_LIST_TOP + (int)WIFI_SCAN_MAX_VISIBLE * (int)WIFI_SCAN_ROW_H) {
                const size_t row = (size_t)((py - WIFI_SCAN_LIST_TOP) / WIFI_SCAN_ROW_H);
                size_t scroll_offset = 0;
                if (wsetup.selected_ap >= WIFI_SCAN_MAX_VISIBLE) {
                    scroll_offset = wsetup.selected_ap - WIFI_SCAN_MAX_VISIBLE + 1;
                }
                const size_t idx = scroll_offset + row;
                if (idx < wsetup.ap_count) {
                    wsetup.selected_ap = idx;
                    strlcpy(wsetup.target_ssid, wsetup.aps[idx].ssid, sizeof(wsetup.target_ssid));
                    wsetup.password[0] = '\0';
                    wsetup.password_len = 0;
                    wsetup.view = WIFI_VIEW_PASSWORD_INPUT;
                    wifi_setup_render(ctx);
                }
            }
        }

        /* 5. Tap Row in Saved Tab */
        if (wsetup.tab == WIFI_TAB_SAVED && wsetup.view == WIFI_VIEW_NORMAL && wsetup.saved_count > 0) {
            if (py >= WIFI_SAVED_LIST_TOP && py < WIFI_SAVED_LIST_TOP + (int)WIFI_SAVED_MAX_VISIBLE * (int)WIFI_SAVED_ROW_H) {
                const size_t row = (size_t)((py - WIFI_SAVED_LIST_TOP) / WIFI_SAVED_ROW_H);
                size_t scroll_offset = 0;
                if (wsetup.selected_saved >= WIFI_SAVED_MAX_VISIBLE) {
                    scroll_offset = wsetup.selected_saved - WIFI_SAVED_MAX_VISIBLE + 1;
                }
                const size_t idx = scroll_offset + row;
                if (idx < wsetup.saved_count) {
                    wsetup.selected_saved = idx;
                    wifi_setup_render(ctx);
                }
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (wsetup.show_help) {
            wsetup.show_help = false;
            wifi_setup_render(ctx);
            return true;
        }
        if (wsetup.view == WIFI_VIEW_NORMAL && solar_os_help_char_opens(ch)) {
            wsetup.show_help = true;
            wifi_setup_render(ctx);
            return true;
        }

        /* Tab Switching */
        if (wsetup.view == WIFI_VIEW_NORMAL && (ch == '\t' || ch == '1' || ch == '2')) {
            if (ch == '1') {
                wsetup.tab = WIFI_TAB_SCAN;
            } else if (ch == '2') {
                wsetup.tab = WIFI_TAB_SAVED;
                wifi_refresh_saved_list();
            } else {
                wsetup.tab = (wsetup.tab == WIFI_TAB_SCAN) ? WIFI_TAB_SAVED : WIFI_TAB_SCAN;
                if (wsetup.tab == WIFI_TAB_SAVED) wifi_refresh_saved_list();
            }
            wifi_setup_render(ctx);
            return true;
        }

        if (wsetup.view == WIFI_VIEW_NORMAL) {
            if (wsetup.tab == WIFI_TAB_SCAN) {
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
            } else {
                /* Saved Networks Tab Key Navigation */
                if (ch == SOLAR_OS_KEY_UP || ch == 'k' || ch == 'K') {
                    if (wsetup.selected_saved > 0) {
                        wsetup.selected_saved--;
                        wifi_setup_render(ctx);
                    }
                    return true;
                }
                if (ch == SOLAR_OS_KEY_DOWN || ch == 'j' || ch == 'J') {
                    if (wsetup.saved_count > 0 && wsetup.selected_saved + 1 < wsetup.saved_count) {
                        wsetup.selected_saved++;
                        wifi_setup_render(ctx);
                    }
                    return true;
                }
                if (ch == 'u' || ch == 'U') {
                    if (wsetup.selected_saved > 0 && wsetup.selected_saved < wsetup.saved_count) {
                        (void)solar_os_wifi_move_profile(wsetup.selected_saved, wsetup.selected_saved - 1);
                        wsetup.selected_saved--;
                        wifi_refresh_saved_list();
                        wifi_setup_render(ctx);
                    }
                    return true;
                }
                if (ch == 'd' || ch == 'D') {
                    if (wsetup.selected_saved + 1 < wsetup.saved_count) {
                        (void)solar_os_wifi_move_profile(wsetup.selected_saved, wsetup.selected_saved + 1);
                        wsetup.selected_saved++;
                        wifi_refresh_saved_list();
                        wifi_setup_render(ctx);
                    }
                    return true;
                }
                if (ch == 't' || ch == 'T' || ch == ' ') {
                    if (wsetup.selected_saved < wsetup.saved_count) {
                        (void)solar_os_wifi_toggle_profile(wsetup.saved[wsetup.selected_saved].ssid);
                        wifi_refresh_saved_list();
                        wifi_setup_render(ctx);
                    }
                    return true;
                }
                if (ch == '\r' || ch == '\n') {
                    if (wsetup.selected_saved < wsetup.saved_count) {
                        const solar_os_wifi_saved_entry_t *entry = &wsetup.saved[wsetup.selected_saved];
                        wsetup.view = WIFI_VIEW_CONNECTING;
                        wsetup.connect_start_ms = (uint32_t)(esp_timer_get_time() / 1000U);
                        strlcpy(wsetup.target_ssid, entry->ssid, sizeof(wsetup.target_ssid));
                        (void)solar_os_wifi_connect(entry->ssid, entry->password);
                        wifi_setup_render(ctx);
                    }
                    return true;
                }
                if (ch == 'x' || ch == 'X' || ch == '\b' || ch == 127) {
                    if (wsetup.selected_saved < wsetup.saved_count) {
                        const solar_os_wifi_saved_entry_t *entry = &wsetup.saved[wsetup.selected_saved];
                        if (entry->is_sd) {
                            (void)solar_os_wifi_delete_sd_entry(entry->ssid);
                        } else {
                            (void)solar_os_wifi_forget_ssid(entry->ssid);
                        }
                        wifi_refresh_saved_list();
                        wifi_setup_render(ctx);
                    }
                    return true;
                }
                if (ch == 'e' || ch == 'E') {
                    (void)solar_os_wifi_save_to_sd_file();
                    snprintf(wsetup.status_msg, sizeof(wsetup.status_msg), "Exported saved profiles to SD card.");
                    wifi_refresh_saved_list();
                    wifi_setup_render(ctx);
                    return true;
                }
                if (ch == 'i' || ch == 'I') {
                    (void)solar_os_wifi_sync_sd_file();
                    snprintf(wsetup.status_msg, sizeof(wsetup.status_msg), "Imported profiles from SD card.");
                    wifi_refresh_saved_list();
                    wifi_setup_render(ctx);
                    return true;
                }
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
                wsetup.view = WIFI_VIEW_NORMAL;
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
