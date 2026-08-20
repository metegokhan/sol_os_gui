#include "solar_os_settings_gui.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "solar_os.h"
#include "solar_os_agent.h"
#include "solar_os_app_registry.h"
#include "solar_os_audio.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_display.h"
#include "solar_os_gfx.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_resource_limits.h"
#include "solar_os_storage.h"
#include "solar_os_time.h"
#include "solar_os_terminal.h"
#include "solar_os_sessions.h"
#include "solar_os_wifi.h"
#include "solar_os_appbar.h"
#include "solar_os_help.h"

#define SETTINGS_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(SETTINGS_STACK_SIZE);

typedef enum {
    CAT_TIME_CITY = 0,
    CAT_DISPLAY = 1,
    CAT_KEYBOARD = 2,
    CAT_AUDIO = 3,
    CAT_WIRELESS = 4,
    CAT_APP_SETTINGS = 5,
    CAT_SYSTEM = 6,
    CAT_COUNT = 7
} settings_category_t;

typedef struct {
    solar_os_context_t *ctx;
    settings_category_t current_cat;
    size_t selected_row;
    char notice_msg[64];
    uint32_t notice_timer_ms;
    bool show_help;

    /* Time & Location */
    int city_idx;
    int tz_idx;
    int ntp_sync;
    int temp_unit;
    int edit_year;
    int edit_month;
    int edit_day;
    int edit_hour;
    int edit_min;

    /* Display */
    int brightness;
    int color_theme;
    int screensaver_timeout;
    int screensaver_mode;

    /* Keyboard */
    int kbd_layout;
    int repeat_delay;
    int repeat_rate;

    /* Audio */
    int master_volume;
    int audio_beeps;

    /* App Settings */
    int chat_channel;
    int web_homepage_idx;
    int agent_provider_idx;
    int clock_city2_idx;
} settings_state_t;

static void *settings_state_ptr;
#define sstate (*(settings_state_t *)settings_state_ptr)

static const char *const cat_names[CAT_COUNT] = {
    "1. Time & City",
    "2. Display",
    "3. Keyboard",
    "4. Audio",
    "5. Wireless",
    "6. App Config",
    "7. Storage & Sys"
};

static const char *const city_options[] = {
    "Istanbul (Turkey)",
    "Ankara (Turkey)",
    "Izmir (Turkey)",
    "London (UK)",
    "Berlin (Germany)",
    "New York (USA)",
    "Tokyo (Japan)",
    "Paris (France)",
};
#define CITY_COUNT (sizeof(city_options)/sizeof(city_options[0]))

static const char *const city_tz_map[] = {
    "Europe/Istanbul",
    "Europe/Istanbul",
    "Europe/Istanbul",
    "Europe/London",
    "Europe/Berlin",
    "America/New_York",
    "Asia/Tokyo",
    "Europe/Paris"
};

static const char *const ntp_options[] = {"Enabled", "Disabled"};
static const char *const temp_unit_options[] = {"Celsius (C)", "Fahrenheit (F)"};

static const char *const theme_options[] = {"Standard Light", "Inverted Dark"};
static const char *const ss_timeout_options[] = {"30 Seconds", "1 Minute (Default)", "2 Minutes", "5 Minutes", "Disabled"};
static const char *const ss_mode_options[] = {"Clock, Calendar & Weather", "Live Clock & Calendar", "Photo Frame Slideshow"};

static const char *const kbd_layout_options[] = {"Turkish Q (TR)", "US English (US)", "German (DE)"};
static const char *const repeat_delay_options[] = {"250 ms", "500 ms (Default)", "750 ms", "1000 ms"};
static const char *const repeat_rate_options[] = {"15 cps", "30 cps (Default)", "50 cps"};

static const char *const beeps_options[] = {"Enabled", "Disabled"};

static const char *const web_home_options[] = {"https://lite.duckduckgo.com", "https://en.wikipedia.org", "https://news.ycombinator.com", "https://open-meteo.com"};
static const char *const agent_prov_options[] = {"OpenAI (GPT-4o-mini)", "DeepSeek (V3-Chat)", "Local Server (Ollama)"};
static const char *const chat_chan_options[] = {"CH 1 (Default - 433.175 MHz)", "CH 2 (433.375 MHz)", "CH 3 (433.575 MHz)", "CH 4 (Emergency)"};

static void settings_apply_changes(void)
{
    /* 1. Timezone */
    if (sstate.city_idx >= 0 && sstate.city_idx < (int)CITY_COUNT) {
        (void)solar_os_time_set_timezone(city_tz_map[sstate.city_idx]);
    }

    /* 2. Keyboard Layout */
    if (sstate.kbd_layout == 0) {
        (void)solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_TR);
    } else if (sstate.kbd_layout == 1) {
        (void)solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US);
    } else {
        (void)solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE);
    }

    /* 3. Audio Volume */
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    (void)solar_os_audio_set_volume((uint8_t)sstate.master_volume);
#endif

    /* 4. Display Settings (Brightness & Color Theme) */
#if SOLAR_OS_PACKAGE_SERVICE_DISPLAY
    if (solar_os_display_brightness_supported()) {
        (void)solar_os_display_set_brightness((uint8_t)sstate.brightness);
    }
    const bool inverted = (sstate.color_theme == 1);
    (void)solar_os_terminal_set_palette_preference(inverted);
    (void)solar_os_display_set_palette_inverted(NULL, inverted);
    (void)solar_os_display_set_controller_mode(NULL, inverted ? "inverted=on" : "inverted=off");
    if (sstate.ctx != NULL) {
        solar_os_terminal_t *term = solar_os_context_terminal(sstate.ctx);
        if (term != NULL) {
            (void)solar_os_sessions_set_terminal_palette_inverted(term, inverted);
        }
    }
#endif

    /* 5. Web Homepage */
    if (sstate.web_homepage_idx >= 0 && sstate.web_homepage_idx < (int)(sizeof(web_home_options)/sizeof(web_home_options[0]))) {
        nvs_handle_t nvs;
        if (nvs_open("web", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, "homepage", web_home_options[sstate.web_homepage_idx]);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }

    /* 6. AI Agent Provider */
    if (sstate.agent_provider_idx == 0) {
        (void)solar_os_agent_set_endpoint("https://api.openai.com/v1/chat/completions");
        (void)solar_os_agent_set_model("gpt-4o-mini");
    } else if (sstate.agent_provider_idx == 1) {
        (void)solar_os_agent_set_endpoint("https://api.deepseek.com/chat/completions");
        (void)solar_os_agent_set_model("deepseek-chat");
    } else if (sstate.agent_provider_idx == 2) {
        (void)solar_os_agent_set_endpoint("http://192.168.1.100:11434/v1/chat/completions");
        (void)solar_os_agent_set_model("llama3.2");
    }
}

static void settings_save_manual_time(void)
{
    solar_os_datetime_t dt = {
        .year = (uint16_t)sstate.edit_year,
        .month = (uint8_t)sstate.edit_month,
        .day = (uint8_t)sstate.edit_day,
        .hour = (uint8_t)sstate.edit_hour,
        .minute = (uint8_t)sstate.edit_min,
        .second = 0,
        .clock_integrity = true,
    };
    (void)solar_os_time_set_datetime(&dt);
    strlcpy(sstate.notice_msg, "System Date & Time updated successfully!", sizeof(sstate.notice_msg));
}

static void settings_trigger_ntp_sync(void)
{
    solar_os_wifi_status_t wst;
    solar_os_wifi_get_status(&wst);
    if (!wst.connected || !wst.has_ip) {
        strlcpy(sstate.notice_msg, "NTP Sync Failed: Wi-Fi not connected!", sizeof(sstate.notice_msg));
        return;
    }

    solar_os_datetime_t utc, local;
    esp_err_t err = solar_os_time_ntp_sync("pool.ntp.org", 8000U, &utc, &local);
    if (err == ESP_OK) {
        sstate.edit_year = local.year;
        sstate.edit_month = local.month;
        sstate.edit_day = local.day;
        sstate.edit_hour = local.hour;
        sstate.edit_min = local.minute;
        snprintf(sstate.notice_msg, sizeof(sstate.notice_msg), "NTP Synced! Time: %02d:%02d:%02d", local.hour, local.minute, local.second);
    } else {
        strlcpy(sstate.notice_msg, "NTP Sync Timeout. Check internet connection.", sizeof(sstate.notice_msg));
    }
}

static int settings_row_count(settings_category_t c)
{
    switch (c) {
    case CAT_TIME_CITY:    return 6;
    case CAT_DISPLAY:      return 4;
    case CAT_KEYBOARD:     return 3;
    case CAT_AUDIO:        return 2;
    case CAT_WIRELESS:     return 3;
    case CAT_APP_SETTINGS: return 4;
    case CAT_SYSTEM:       return 4;
    default:               return 0;
    }
}

/* Rows that trigger an action on ENTER rather than adjusting a value. */
static bool settings_row_is_action(settings_category_t c, size_t r)
{
    if (c == CAT_TIME_CITY) return r == 1 || r == 4;
    if (c == CAT_WIRELESS)  return r == 1 || r == 2;
    if (c == CAT_SYSTEM)    return r == 1 || r == 2 || r == 3;
    return false;
}

/* Rows whose value can be changed with the < / > controls. */
static bool settings_row_is_adjustable(settings_category_t c, size_t r)
{
    if (settings_row_is_action(c, r)) return false;
    if (c == CAT_WIRELESS && r == 0) return false; /* Wi-Fi status (read-only) */
    if (c == CAT_SYSTEM && r == 0) return false;   /* storage status (read-only) */
    return true;
}

/* Draws a row's value centered, with tappable < and > controls pinned to the
 * ends when the row is adjustable. Caller sets the text color first. */
static void settings_draw_value_line(solar_os_gfx_t *gfx, int content_x, int row_w,
                                     int ty, const char *value, bool adjustable)
{
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    const int vw = (int)solar_os_gfx_text_width(gfx, value);
    int vx = content_x + (row_w - vw) / 2;
    if (vx < content_x + 22) vx = content_x + 22;
    solar_os_gfx_text(gfx, vx, ty, value);
    if (adjustable) {
        solar_os_gfx_text(gfx, content_x + 6, ty, "<");
        const int gw = (int)solar_os_gfx_text_width(gfx, ">");
        solar_os_gfx_text(gfx, content_x + row_w - gw - 8, ty, ">");
    }
}

static const char *const settings_help_lines[] = {
    "Left panel: setting categories. Right panel: that",
    "category's settings.",
    "",
    "Touch:",
    "  - Tap a category on the left to open it.",
    "  - Tap a value row's left half to decrease, right",
    "    half to increase.",
    "  - Tap an action row (Sync, Save, Open...) to run it.",
    "",
    "Keyboard:",
    "  Tab: next category    Up/Down: move row",
    "  Left/Right: change value    Enter: run action",
    "  Esc / Q: exit (changes are applied on exit)",
};
#define SETTINGS_HELP_LINE_COUNT (sizeof(settings_help_lines) / sizeof(settings_help_lines[0]))

static void settings_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Shared header. */
    solar_os_appbar_header_t header = {0};
    header.title = "Settings";
    header.show_back = true;
    solar_os_appbar_draw_header(gfx, &header);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    /* 2. Left Sidebar: Categories (X: 0..115, Y: 24..276) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 118, 24, 118, 276);

    const int cat_y_start = 26;
    const int cat_h = 35;

    for (int i = 0; i < (int)CAT_COUNT; i++) {
        const int cy = cat_y_start + i * cat_h;
        const bool is_active = (i == (int)sstate.current_cat);

        if (is_active) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 4, cy, 110, 30);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }

        solar_os_gfx_set_font(gfx, is_active ? SOLAR_OS_GFX_FONT_BOLD : SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 8, cy + 18, cat_names[i]);
    }

    /* 3. Right Content Area: Settings List (X: 125..390, Y: 30..250) */
    const int content_x = 126;
    const int row_y_start = 30;
    const int row_h = 32;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

    if (sstate.current_cat == CAT_TIME_CITY) {
        char cur_time_str[48];
        snprintf(cur_time_str, sizeof(cur_time_str), "Date: %04d-%02d-%02d  Time: %02d:%02d",
                 sstate.edit_year, sstate.edit_month, sstate.edit_day, sstate.edit_hour, sstate.edit_min);

        const char *labels[] = {
            "Selected City:",
            "Sync Time via Internet (NTP Now)",
            "Manual Set Year / Month / Day:",
            "Manual Set Hour / Minute:",
            "Save Manual Date & Time to Clock",
            "Temperature Unit:"
        };
        char year_str[32], time_str[32];
        snprintf(year_str, sizeof(year_str), "%04d-%02d-%02d", sstate.edit_year, sstate.edit_month, sstate.edit_day);
        snprintf(time_str, sizeof(time_str), "%02d:%02d", sstate.edit_hour, sstate.edit_min);

        const char *vals[] = {
            city_options[sstate.city_idx],
            "[ Press ENTER to Sync over Wi-Fi ]",
            year_str,
            time_str,
            "[ Press ENTER to Save Time ]",
            temp_unit_options[sstate.temp_unit]
        };

        for (size_t r = 0; r < 6; r++) {
            const int ry = row_y_start + (int)r * row_h;
            const bool is_sel = (r == sstate.selected_row);

            if (is_sel) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, content_x, ry, screen_w - content_x - 10, row_h - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            }

            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, content_x + 6, ry + 12, labels[r]);
            settings_draw_value_line(gfx, content_x, screen_w - content_x - 10, ry + 24,
                                     vals[r], settings_row_is_adjustable(sstate.current_cat, r));
        }
    } else if (sstate.current_cat == CAT_DISPLAY) {
        const char *labels[] = {"Brightness:", "Color Theme:", "Screensaver Timeout:", "Screensaver Mode:"};
        char b_val[32];
        snprintf(b_val, sizeof(b_val), "%d%%", sstate.brightness);
        const char *vals[] = {
            b_val,
            theme_options[sstate.color_theme],
            ss_timeout_options[sstate.screensaver_timeout],
            ss_mode_options[sstate.screensaver_mode]
        };
        for (size_t r = 0; r < 4; r++) {
            const int ry = row_y_start + (int)r * row_h;
            const bool is_sel = (r == sstate.selected_row);

            if (is_sel) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, content_x, ry, screen_w - content_x - 10, row_h - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            }

            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, content_x + 6, ry + 12, labels[r]);
            settings_draw_value_line(gfx, content_x, screen_w - content_x - 10, ry + 24,
                                     vals[r], settings_row_is_adjustable(sstate.current_cat, r));
        }
    } else if (sstate.current_cat == CAT_KEYBOARD) {
        const char *labels[] = {"Keyboard Layout:", "Repeat Delay:", "Repeat Rate:"};
        const char *vals[] = {
            kbd_layout_options[sstate.kbd_layout],
            repeat_delay_options[sstate.repeat_delay],
            repeat_rate_options[sstate.repeat_rate]
        };
        for (size_t r = 0; r < 3; r++) {
            const int ry = row_y_start + (int)r * row_h;
            const bool is_sel = (r == sstate.selected_row);

            if (is_sel) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, content_x, ry, screen_w - content_x - 10, row_h - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            }

            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, content_x + 6, ry + 12, labels[r]);
            settings_draw_value_line(gfx, content_x, screen_w - content_x - 10, ry + 24,
                                     vals[r], settings_row_is_adjustable(sstate.current_cat, r));
        }
    } else if (sstate.current_cat == CAT_AUDIO) {
        const char *labels[] = {"Master Volume:", "Notification Beeps:"};
        char v_val[32];
        snprintf(v_val, sizeof(v_val), "%d%%", sstate.master_volume);
        const char *vals[] = {
            v_val,
            beeps_options[sstate.audio_beeps]
        };
        for (size_t r = 0; r < 2; r++) {
            const int ry = row_y_start + (int)r * row_h;
            const bool is_sel = (r == sstate.selected_row);

            if (is_sel) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, content_x, ry, screen_w - content_x - 10, row_h - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            }

            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, content_x + 6, ry + 12, labels[r]);
            settings_draw_value_line(gfx, content_x, screen_w - content_x - 10, ry + 24,
                                     vals[r], settings_row_is_adjustable(sstate.current_cat, r));
        }
    } else if (sstate.current_cat == CAT_WIRELESS) {
        solar_os_wifi_status_t wst;
        solar_os_wifi_get_status(&wst);
        char wifi_info[64];
        if (wst.connected && wst.has_ip) {
            snprintf(wifi_info, sizeof(wifi_info), "Connected: %s (%s)", wst.saved_ap_ssid[0] ? wst.saved_ap_ssid : "Active", wst.ip);
        } else {
            snprintf(wifi_info, sizeof(wifi_info), "Disconnected / Offline");
        }

        const char *labels[] = {"Wi-Fi Status:", "Launch Wi-Fi Setup Wizard", "Start BLE Keyboard Pairing (123456)"};
        const char *vals[] = {wifi_info, "[ Press ENTER to Open ]", "[ Press ENTER to Pair ]"};

        for (size_t r = 0; r < 3; r++) {
            const int ry = row_y_start + (int)r * row_h;
            const bool is_sel = (r == sstate.selected_row);

            if (is_sel) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, content_x, ry, screen_w - content_x - 10, row_h - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            }

            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, content_x + 6, ry + 12, labels[r]);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, content_x + 6, ry + 24, vals[r]);
        }
    } else if (sstate.current_cat == CAT_APP_SETTINGS) {
        const char *labels[] = {
            "Mesh Chat Channel:",
            "Web Browser Homepage:",
            "AI Agent Provider:",
            "World Clock City 2:"
        };
        const char *vals[] = {
            chat_chan_options[sstate.chat_channel % 4],
            web_home_options[sstate.web_homepage_idx % 4],
            agent_prov_options[sstate.agent_provider_idx % 3],
            city_options[sstate.clock_city2_idx % (int)CITY_COUNT]
        };

        for (size_t r = 0; r < 4; r++) {
            const int ry = row_y_start + (int)r * row_h;
            const bool is_sel = (r == sstate.selected_row);

            if (is_sel) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, content_x, ry, screen_w - content_x - 10, row_h - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            }

            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, content_x + 6, ry + 12, labels[r]);
            settings_draw_value_line(gfx, content_x, screen_w - content_x - 10, ry + 24,
                                     vals[r], settings_row_is_adjustable(sstate.current_cat, r));
        }
    } else if (sstate.current_cat == CAT_SYSTEM) {
        char sd_info[64];
        snprintf(sd_info, sizeof(sd_info), "SD Card: %s (/sdcard)", solar_os_storage_sd_is_mounted() ? "Mounted (Ready)" : "Not Detected");

        const char *labels[] = {
            "Storage State:",
            "Start Wi-Fi Web File Server",
            "Open SolarOS CLI Terminal",
            "Restart ESP32-S3 Device"
        };
        const char *vals[] = {
            sd_info,
            "[ Press ENTER to Start HTTP Server ]",
            "[ Press ENTER to Exit to Shell ]",
            "[ Press ENTER to Reboot ]"
        };

        for (size_t r = 0; r < 4; r++) {
            const int ry = row_y_start + (int)r * row_h;
            const bool is_sel = (r == sstate.selected_row);

            if (is_sel) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, content_x, ry, screen_w - content_x - 10, row_h - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            }

            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, content_x + 6, ry + 12, labels[r]);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, content_x + 6, ry + 24, vals[r]);
        }
    }

    /* 4. Notice / Status Bar (Y: 254) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 6, 252, screen_w - 6, 252);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    if (sstate.notice_msg[0] != '\0') {
        solar_os_gfx_text(gfx, 12, 268, sstate.notice_msg);
    } else {
        solar_os_gfx_text(gfx, 12, 268, "Tap a category, then a row to adjust it.");
    }

    /* 5. Footer: shared Help chip. */
    solar_os_appbar_shortcut_t fitems[1];
    solar_os_help_chip(&fitems[0]);
    const solar_os_appbar_shortcuts_t fbar = { .items = fitems, .count = 1 };
    solar_os_appbar_draw_footer(gfx, &fbar);

    /* Help overlay on top of everything. */
    if (sstate.show_help) {
        solar_os_help_draw(gfx, "Settings - Help",
                           settings_help_lines, SETTINGS_HELP_LINE_COUNT);
    }

    solar_os_gfx_present(gfx);
}

static esp_err_t settings_start(solar_os_context_t *ctx)
{
    sstate.current_cat = CAT_TIME_CITY;
    sstate.selected_row = 0;
    sstate.notice_msg[0] = '\0';

    sstate.city_idx = 0;
    sstate.tz_idx = 0;
    sstate.ntp_sync = 0;
    sstate.temp_unit = 0;

    time_t raw = time(NULL);
    struct tm *lt = localtime(&raw);
    if (lt != NULL && lt->tm_year > 100) {
        sstate.edit_year = lt->tm_year + 1900;
        sstate.edit_month = lt->tm_mon + 1;
        sstate.edit_day = lt->tm_mday;
        sstate.edit_hour = lt->tm_hour;
        sstate.edit_min = lt->tm_min;
    } else {
        sstate.edit_year = 2026;
        sstate.edit_month = 8;
        sstate.edit_day = 14;
        sstate.edit_hour = 23;
        sstate.edit_min = 10;
    }

    sstate.ctx = ctx;
    sstate.brightness = 100;
    sstate.color_theme = solar_os_terminal_palette_preference_inverted() ? 1 : 0;
    sstate.screensaver_timeout = 1;
    sstate.screensaver_mode = 0;

    const solar_os_input_keyboard_layout_t kl = solar_os_input_keyboard_layout();
    if (kl == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_TR) sstate.kbd_layout = 0;
    else if (kl == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US) sstate.kbd_layout = 1;
    else sstate.kbd_layout = 2;

    sstate.repeat_delay = 1;
    sstate.repeat_rate = 1;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    solar_os_audio_status_t astatus;
    solar_os_audio_get_status(&astatus);
    sstate.master_volume = (int)astatus.volume;
#else
    sstate.master_volume = 80;
#endif
    sstate.audio_beeps = 0;

    sstate.chat_channel = 0;
    sstate.web_homepage_idx = 0;
    sstate.agent_provider_idx = 0;
    sstate.clock_city2_idx = 3;

    /* Load saved web homepage */
    nvs_handle_t nvs;
    if (nvs_open("web", NVS_READONLY, &nvs) == ESP_OK) {
        char saved_home[128] = "";
        size_t len = sizeof(saved_home);
        if (nvs_get_str(nvs, "homepage", saved_home, &len) == ESP_OK) {
            for (size_t i = 0; i < sizeof(web_home_options)/sizeof(web_home_options[0]); i++) {
                if (strcmp(saved_home, web_home_options[i]) == 0) {
                    sstate.web_homepage_idx = (int)i;
                    break;
                }
            }
        }
        nvs_close(nvs);
    }

    /* Load saved agent provider */
    solar_os_agent_status_t agent_st;
    if (solar_os_agent_get_status(&agent_st) == ESP_OK) {
        if (strstr(agent_st.endpoint, "deepseek") != NULL) sstate.agent_provider_idx = 1;
        else if (strstr(agent_st.endpoint, "11434") != NULL || strstr(agent_st.endpoint, "ollama") != NULL) sstate.agent_provider_idx = 2;
        else sstate.agent_provider_idx = 0;
    }

    solar_os_context_set_graphics_active(ctx, true);
    settings_render(ctx);
    return ESP_OK;
}

static void settings_stop(solar_os_context_t *ctx)
{
    settings_apply_changes();
    solar_os_context_set_graphics_active(ctx, false);
}

static bool settings_event(solar_os_context_t *ctx, const solar_os_event_t *event);

/* Replays a keyboard char through this same handler so a touch can reuse the
 * existing Left/Right/Enter logic without duplicating it. */
static void settings_feed_char(solar_os_context_t *ctx, char ch)
{
    solar_os_event_t fake = {0};
    fake.type = SOLAR_OS_EVENT_CHAR;
    fake.data.ch = ch;
    (void)settings_event(ctx, &fake);
}

static bool settings_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int screen_w = (int)solar_os_gfx_width(gfx);
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        /* Any tap dismisses the help overlay. */
        if (sstate.show_help) {
            sstate.show_help = false;
            settings_render(ctx);
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

        solar_os_appbar_shortcut_t fitems[1];
        solar_os_help_chip(&fitems[0]);
        const solar_os_appbar_shortcuts_t fbar = { .items = fitems, .count = 1 };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &fbar, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM) {
                sstate.show_help = true;
                settings_render(ctx);
            }
            return true;
        }

        /* Left sidebar: pick a category. */
        if (px >= 4 && px < 116) {
            const int cat_y_start = 26;
            const int cat_h = 35;
            if (py >= cat_y_start) {
                const int i = (py - cat_y_start) / cat_h;
                if (i >= 0 && i < (int)CAT_COUNT) {
                    sstate.current_cat = (settings_category_t)i;
                    sstate.selected_row = 0;
                    sstate.notice_msg[0] = '\0';
                    settings_render(ctx);
                }
            }
            return true;
        }

        /* Content rows: select and act. */
        const int content_x = 126;
        const int row_y_start = 30;
        const int row_h = 32;
        if (px >= content_x && py >= row_y_start) {
            const int r = (py - row_y_start) / row_h;
            const int rc = settings_row_count(sstate.current_cat);
            if (r >= 0 && r < rc) {
                sstate.selected_row = (size_t)r;
                if (settings_row_is_action(sstate.current_cat, (size_t)r)) {
                    settings_feed_char(ctx, '\r');
                } else if (settings_row_is_adjustable(sstate.current_cat, (size_t)r)) {
                    const int mid = content_x + (screen_w - content_x - 10) / 2;
                    settings_feed_char(ctx, (char)(px < mid ? SOLAR_OS_KEY_LEFT : SOLAR_OS_KEY_RIGHT));
                }
                settings_render(ctx);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        /* Help overlay: any key closes it; Ctrl+H / '?' opens it. */
        if (sstate.show_help) {
            sstate.show_help = false;
            settings_render(ctx);
            return true;
        }
        if (solar_os_help_char_opens(ch)) {
            sstate.show_help = true;
            settings_render(ctx);
            return true;
        }

        if (ch == '\t') {
            sstate.current_cat = (sstate.current_cat + 1) % CAT_COUNT;
            sstate.selected_row = 0;
            settings_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W' || ch == 'k' || ch == 'K') {
            if (sstate.selected_row > 0) {
                sstate.selected_row--;
                settings_render(ctx);
            }
            return true;
        }

        if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S' || ch == 'j' || ch == 'J') {
            sstate.selected_row++;
            if (sstate.current_cat == CAT_TIME_CITY && sstate.selected_row >= 6) sstate.selected_row = 5;
            if (sstate.current_cat == CAT_DISPLAY && sstate.selected_row >= 4) sstate.selected_row = 3;
            if (sstate.current_cat == CAT_KEYBOARD && sstate.selected_row >= 3) sstate.selected_row = 2;
            if (sstate.current_cat == CAT_AUDIO && sstate.selected_row >= 2) sstate.selected_row = 1;
            if (sstate.current_cat == CAT_WIRELESS && sstate.selected_row >= 3) sstate.selected_row = 2;
            if (sstate.current_cat == CAT_APP_SETTINGS && sstate.selected_row >= 4) sstate.selected_row = 3;
            if (sstate.current_cat == CAT_SYSTEM && sstate.selected_row >= 4) sstate.selected_row = 3;
            settings_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A' || ch == 'h' || ch == 'H') {
            if (sstate.current_cat == CAT_TIME_CITY) {
                if (sstate.selected_row == 0) {
                    if (sstate.city_idx > 0) sstate.city_idx--;
                    else sstate.city_idx = (int)CITY_COUNT - 1;
                    settings_apply_changes();
                } else if (sstate.selected_row == 2) {
                    if (sstate.edit_day > 1) sstate.edit_day--;
                    else if (sstate.edit_month > 1) { sstate.edit_month--; sstate.edit_day = 28; }
                    else if (sstate.edit_year > 2024) { sstate.edit_year--; sstate.edit_month = 12; sstate.edit_day = 31; }
                } else if (sstate.selected_row == 3) {
                    if (sstate.edit_min >= 5) sstate.edit_min -= 5;
                    else if (sstate.edit_hour > 0) { sstate.edit_hour--; sstate.edit_min = 55; }
                } else if (sstate.selected_row == 5) {
                    sstate.temp_unit = (sstate.temp_unit == 0) ? 1 : 0;
                }
            } else if (sstate.current_cat == CAT_DISPLAY) {
                if (sstate.selected_row == 0 && sstate.brightness > 10) {
                    sstate.brightness -= 10;
                    settings_apply_changes();
                } else if (sstate.selected_row == 1) {
                    sstate.color_theme = (sstate.color_theme == 0) ? 1 : 0;
                    settings_apply_changes();
                } else if (sstate.selected_row == 2 && sstate.screensaver_timeout > 0) {
                    sstate.screensaver_timeout--;
                } else if (sstate.selected_row == 3 && sstate.screensaver_mode > 0) {
                    sstate.screensaver_mode--;
                }
            } else if (sstate.current_cat == CAT_KEYBOARD) {
                if (sstate.selected_row == 0) {
                    if (sstate.kbd_layout > 0) sstate.kbd_layout--;
                    else sstate.kbd_layout = 2;
                    settings_apply_changes();
                } else if (sstate.selected_row == 1 && sstate.repeat_delay > 0) {
                    sstate.repeat_delay--;
                } else if (sstate.selected_row == 2 && sstate.repeat_rate > 0) {
                    sstate.repeat_rate--;
                }
            } else if (sstate.current_cat == CAT_AUDIO) {
                if (sstate.selected_row == 0 && sstate.master_volume >= 10) {
                    sstate.master_volume -= 10;
                    settings_apply_changes();
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
                    (void)solar_os_audio_play_tone(880, 100, (uint8_t)sstate.master_volume);
#endif
                } else if (sstate.selected_row == 1) {
                    sstate.audio_beeps = (sstate.audio_beeps == 0) ? 1 : 0;
                }
            } else if (sstate.current_cat == CAT_APP_SETTINGS) {
                if (sstate.selected_row == 0) {
                    if (sstate.chat_channel > 0) sstate.chat_channel--;
                    else sstate.chat_channel = 3;
                } else if (sstate.selected_row == 1) {
                    if (sstate.web_homepage_idx > 0) sstate.web_homepage_idx--;
                    else sstate.web_homepage_idx = 3;
                } else if (sstate.selected_row == 2) {
                    if (sstate.agent_provider_idx > 0) sstate.agent_provider_idx--;
                    else sstate.agent_provider_idx = 2;
                } else if (sstate.selected_row == 3) {
                    if (sstate.clock_city2_idx > 0) sstate.clock_city2_idx--;
                    else sstate.clock_city2_idx = (int)CITY_COUNT - 1;
                }
            }
            settings_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D' || ch == 'l' || ch == 'L') {
            if (sstate.current_cat == CAT_TIME_CITY) {
                if (sstate.selected_row == 0) {
                    sstate.city_idx = (sstate.city_idx + 1) % (int)CITY_COUNT;
                    settings_apply_changes();
                } else if (sstate.selected_row == 2) {
                    if (sstate.edit_day < 28) sstate.edit_day++;
                    else if (sstate.edit_month < 12) { sstate.edit_month++; sstate.edit_day = 1; }
                    else if (sstate.edit_year < 2035) { sstate.edit_year++; sstate.edit_month = 1; sstate.edit_day = 1; }
                } else if (sstate.selected_row == 3) {
                    if (sstate.edit_min <= 50) sstate.edit_min += 5;
                    else if (sstate.edit_hour < 23) { sstate.edit_hour++; sstate.edit_min = 0; }
                } else if (sstate.selected_row == 5) {
                    sstate.temp_unit = (sstate.temp_unit == 0) ? 1 : 0;
                }
            } else if (sstate.current_cat == CAT_DISPLAY) {
                if (sstate.selected_row == 0 && sstate.brightness < 100) {
                    sstate.brightness += 10;
                    settings_apply_changes();
                } else if (sstate.selected_row == 1) {
                    sstate.color_theme = (sstate.color_theme == 0) ? 1 : 0;
                    settings_apply_changes();
                } else if (sstate.selected_row == 2 && sstate.screensaver_timeout < 4) {
                    sstate.screensaver_timeout++;
                } else if (sstate.selected_row == 3 && sstate.screensaver_mode < 2) {
                    sstate.screensaver_mode++;
                }
            } else if (sstate.current_cat == CAT_KEYBOARD) {
                if (sstate.selected_row == 0) {
                    sstate.kbd_layout = (sstate.kbd_layout + 1) % 3;
                    settings_apply_changes();
                } else if (sstate.selected_row == 1 && sstate.repeat_delay < 3) {
                    sstate.repeat_delay++;
                } else if (sstate.selected_row == 2 && sstate.repeat_rate < 2) {
                    sstate.repeat_rate++;
                }
            } else if (sstate.current_cat == CAT_AUDIO) {
                if (sstate.selected_row == 0 && sstate.master_volume <= 90) {
                    sstate.master_volume += 10;
                    settings_apply_changes();
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
                    (void)solar_os_audio_play_tone(880, 100, (uint8_t)sstate.master_volume);
#endif
                } else if (sstate.selected_row == 1) {
                    sstate.audio_beeps = (sstate.audio_beeps == 0) ? 1 : 0;
                }
            } else if (sstate.current_cat == CAT_APP_SETTINGS) {
                if (sstate.selected_row == 0) {
                    sstate.chat_channel = (sstate.chat_channel + 1) % 4;
                } else if (sstate.selected_row == 1) {
                    sstate.web_homepage_idx = (sstate.web_homepage_idx + 1) % 4;
                } else if (sstate.selected_row == 2) {
                    sstate.agent_provider_idx = (sstate.agent_provider_idx + 1) % 3;
                } else if (sstate.selected_row == 3) {
                    sstate.clock_city2_idx = (sstate.clock_city2_idx + 1) % (int)CITY_COUNT;
                }
            }
            settings_render(ctx);
            return true;
        }

        if (ch == '\r' || ch == '\n') {
            if (sstate.current_cat == CAT_TIME_CITY) {
                if (sstate.selected_row == 1) {
                    settings_trigger_ntp_sync();
                    settings_render(ctx);
                } else if (sstate.selected_row == 4) {
                    settings_save_manual_time();
                    settings_render(ctx);
                }
            } else if (sstate.current_cat == CAT_WIRELESS) {
                if (sstate.selected_row == 1) {
                    const solar_os_app_registry_entry_t *entry = solar_os_app_registry_find("wifi_setup");
                    if (entry != NULL && entry->app != NULL) {
                        char *argv[] = {"wifi_setup"};
                        solar_os_context_set_graphics_active(ctx, false);
                        return solar_os_context_request_launch_ex(ctx, entry->app, 1, argv, SOLAR_OS_LAUNCH_CHILD_RETURN);
                    }
                } else if (sstate.selected_row == 2) {
#if SOLAR_OS_PACKAGE_SERVICE_BLE
                    (void)solar_os_ble_keyboard_start_pairing();
                    strlcpy(sstate.notice_msg, "BLE Pairing Started. PIN: 123456", sizeof(sstate.notice_msg));
                    settings_render(ctx);
#endif
                }
            } else if (sstate.current_cat == CAT_SYSTEM) {
                if (sstate.selected_row == 1) {
                    const solar_os_app_registry_entry_t *entry = solar_os_app_registry_find("web_files");
                    if (entry != NULL && entry->app != NULL) {
                        char *argv[] = {"web_files"};
                        solar_os_context_set_graphics_active(ctx, false);
                        return solar_os_context_request_launch_ex(ctx, entry->app, 1, argv, SOLAR_OS_LAUNCH_CHILD_RETURN);
                    }
                } else if (sstate.selected_row == 2) {
                    solar_os_context_request_exit(ctx);
                    return true;
                } else if (sstate.selected_row == 3) {
                    esp_restart();
                    return true;
                }
            }
            return true;
        }

        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_settings_gui_app = {
    .name = "settings_gui",
    .summary = "full graphical system settings and control panel",
    .flags = 0,
    .start = settings_start,
    .stop = settings_stop,
    .event = settings_event,
    .state_slot = &settings_state_ptr,
    .state_size = sizeof(settings_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = SETTINGS_STACK_SIZE,
};
