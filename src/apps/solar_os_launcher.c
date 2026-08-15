#include "solar_os_launcher.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_app_registry.h"
#include "solar_os_battery.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_config.h"
#include "solar_os_gfx.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_resource_limits.h"
#include "solar_os_storage.h"
#include "solar_os_time.h"
#include "solar_os_wifi.h"

#define LAUNCHER_FOLDER_COUNT 6
#define LAUNCHER_MAX_ITEMS_PER_FOLDER 36
#define LAUNCHER_MAX_ROMS 24
#define LAUNCHER_MAX_PATH 128
#define LAUNCHER_MAX_LABEL 24
#define LAUNCHER_MAX_DESC 64

#define LAUNCHER_GRID_COLS 3
#define LAUNCHER_GRID_ROWS 2

#define LAUNCHER_CARD_W 108
#define LAUNCHER_CARD_H 94
#define LAUNCHER_GRID_START_X 22
#define LAUNCHER_GRID_START_Y 52
#define LAUNCHER_GRID_GAP_X 16
#define LAUNCHER_GRID_GAP_Y 12

#define LAUNCHER_SUB_GRID_COLS 4
#define LAUNCHER_SUB_CARD_W 84
#define LAUNCHER_SUB_CARD_H 90
#define LAUNCHER_SUB_START_X 18
#define LAUNCHER_SUB_START_Y 52
#define LAUNCHER_SUB_GAP_X 12
#define LAUNCHER_SUB_GAP_Y 12
#define LAUNCHER_SUB_PAGE_SIZE (LAUNCHER_SUB_GRID_COLS * LAUNCHER_GRID_ROWS)

#define LAUNCHER_TASK_STACK 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(LAUNCHER_TASK_STACK);

typedef enum {
    VIEW_HOME_FOLDERS = 0,
    VIEW_INSIDE_FOLDER = 1,
    VIEW_ROM_PICKER = 2,
    VIEW_SCREENSAVER = 3,
} launcher_view_mode_t;

typedef enum {
    ICON_TYPE_FOLDER_SYSTEM,
    ICON_TYPE_FOLDER_TEXT,
    ICON_TYPE_FOLDER_MEDIA,
    ICON_TYPE_FOLDER_NETWORK,
    ICON_TYPE_FOLDER_SCRIPT,
    ICON_TYPE_FOLDER_SETTINGS,
    ICON_TYPE_FILES,
    ICON_TYPE_CALC,
    ICON_TYPE_SHEET,
    ICON_TYPE_PLOT,
    ICON_TYPE_LOGIC,
    ICON_TYPE_THERMOMETER,
    ICON_TYPE_CLOCK,
    ICON_TYPE_CALENDAR,
    ICON_TYPE_WRITER,
    ICON_TYPE_READER,
    ICON_TYPE_NOTES,
    ICON_TYPE_EDIT,
    ICON_TYPE_DOCS,
    ICON_TYPE_GAMEBOY,
    ICON_TYPE_INVADERS,
    ICON_TYPE_CHESS,
    ICON_TYPE_GO,
    ICON_TYPE_SUDOKU,
    ICON_TYPE_POMODORO,
    ICON_TYPE_PLAYER,
    ICON_TYPE_RECORDER,
    ICON_TYPE_PHOTOS,
    ICON_TYPE_SYNTH,
    ICON_TYPE_WEBRADIO,
    ICON_TYPE_VIEW,
    ICON_TYPE_STOPWATCH,
    ICON_TYPE_TIMER,
    ICON_TYPE_WEATHER,
    ICON_TYPE_CHAT,
    ICON_TYPE_WEB,
    ICON_TYPE_EMAIL,
    ICON_TYPE_SSH,
    ICON_TYPE_AGENT,
    ICON_TYPE_PYTHON,
    ICON_TYPE_LUA,
    ICON_TYPE_SETTINGS_CP,
    ICON_TYPE_WIFI_SETUP,
    ICON_TYPE_FILE_SERVER,
    ICON_TYPE_BLE,
    ICON_TYPE_KEYBOARD_LAYOUT,
    ICON_TYPE_REBOOT,
    ICON_TYPE_SHELL,
    ICON_TYPE_GENERIC,
} launcher_icon_type_t;

typedef enum {
    ITEM_KIND_BUILTIN = 0,
    ITEM_KIND_GAMEBOY_ROM = 1,
    ITEM_KIND_GAMEBOY_LAUNCHER = 2,
    ITEM_KIND_PYTHON = 3,
    ITEM_KIND_LUA = 4,
    ITEM_KIND_ACTION_BLE_PAIR = 5,
    ITEM_KIND_ACTION_KEYBOARD_LAYOUT = 6,
    ITEM_KIND_ACTION_SHELL = 7,
    ITEM_KIND_ACTION_REBOOT = 8,
} launcher_item_kind_t;

typedef struct {
    launcher_item_kind_t kind;
    launcher_icon_type_t icon_type;
    char name[LAUNCHER_MAX_LABEL];
    char display_label[LAUNCHER_MAX_LABEL];
    char description[LAUNCHER_MAX_DESC];
    char path[LAUNCHER_MAX_PATH];
} launcher_item_t;

typedef struct {
    char title[LAUNCHER_MAX_LABEL];
    char description[LAUNCHER_MAX_DESC];
    launcher_icon_type_t icon_type;
    launcher_item_t items[LAUNCHER_MAX_ITEMS_PER_FOLDER];
    size_t count;
} launcher_folder_t;

typedef struct {
    char name[48];
    char path[LAUNCHER_MAX_PATH];
    launcher_icon_type_t icon_type;
    bool is_new_file;
} launcher_picker_entry_t;

typedef struct {
    solar_os_context_t *ctx;
    bool active;
    launcher_view_mode_t view_mode;
    size_t selected_folder;
    size_t selected_item[LAUNCHER_FOLDER_COUNT];
    launcher_folder_t folders[LAUNCHER_FOLDER_COUNT];

    /* Universal File Picker Modal State */
    char picker_app_name[24];
    char picker_title[48];
    launcher_icon_type_t picker_icon;
    launcher_picker_entry_t picker_items[64];
    size_t picker_count;
    size_t selected_picker_item;

    /* Screensaver State */
    uint32_t idle_start_ms;
    uint32_t screensaver_timeout_ms;

    char notice_msg[64];
    uint32_t notice_until_ms;
    uint32_t last_tick_ms;
} launcher_state_t;

static void *launcher_state_ptr;
#define launcher (*(launcher_state_t *)launcher_state_ptr)

/* ----------------- 24x24 Vector Icon Renderers ----------------- */

static void draw_app_icon(solar_os_gfx_t *gfx, int cx, int cy, launcher_icon_type_t type, bool selected)
{
    solar_os_gfx_color_t fg = selected ? SOLAR_OS_GFX_COLOR_WHITE : SOLAR_OS_GFX_COLOR_BLACK;
    solar_os_gfx_color_t bg = selected ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_WHITE;
    solar_os_gfx_set_color(gfx, fg);

    switch (type) {
    case ICON_TYPE_FOLDER_SYSTEM:
    case ICON_TYPE_FOLDER_TEXT:
    case ICON_TYPE_FOLDER_MEDIA:
    case ICON_TYPE_FOLDER_NETWORK:
    case ICON_TYPE_FOLDER_SCRIPT:
    case ICON_TYPE_FOLDER_SETTINGS:
        solar_os_gfx_rect(gfx, cx - 14, cy - 10, 28, 22);
        solar_os_gfx_fill_rect(gfx, cx - 14, cy - 14, 12, 4);
        solar_os_gfx_line(gfx, cx - 14, cy - 4, cx + 13, cy - 4);
        if (type == ICON_TYPE_FOLDER_SYSTEM) {
            solar_os_gfx_fill_rect(gfx, cx - 4, cy + 1, 8, 8);
            solar_os_gfx_line(gfx, cx, cy - 1, cx, cy + 10);
            solar_os_gfx_line(gfx, cx - 6, cy + 5, cx + 5, cy + 5);
        } else if (type == ICON_TYPE_FOLDER_TEXT) {
            solar_os_gfx_line(gfx, cx - 6, cy + 1, cx + 6, cy + 1);
            solar_os_gfx_line(gfx, cx - 6, cy + 4, cx + 6, cy + 4);
            solar_os_gfx_line(gfx, cx - 6, cy + 7, cx + 2, cy + 7);
        } else if (type == ICON_TYPE_FOLDER_MEDIA) {
            solar_os_gfx_fill_circle(gfx, cx - 3, cy + 6, 2);
            solar_os_gfx_fill_circle(gfx, cx + 4, cy + 4, 2);
            solar_os_gfx_line(gfx, cx - 1, cy + 6, cx - 1, cy - 1);
            solar_os_gfx_line(gfx, cx + 6, cy + 4, cx + 6, cy - 3);
            solar_os_gfx_line(gfx, cx - 1, cy - 1, cx + 6, cy - 3);
        } else if (type == ICON_TYPE_FOLDER_NETWORK) {
            solar_os_gfx_circle(gfx, cx, cy + 4, 5);
            solar_os_gfx_line(gfx, cx, cy - 1, cx, cy - 5);
            solar_os_gfx_line(gfx, cx - 4, cy - 4, cx + 4, cy - 4);
        } else if (type == ICON_TYPE_FOLDER_SCRIPT) {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, cx - 7, cy + 8, "{ }");
        } else if (type == ICON_TYPE_FOLDER_SETTINGS) {
            solar_os_gfx_circle(gfx, cx, cy + 4, 5);
            solar_os_gfx_fill_circle(gfx, cx, cy + 4, 2);
            solar_os_gfx_line(gfx, cx, cy - 3, cx, cy + 11);
            solar_os_gfx_line(gfx, cx - 7, cy + 4, cx + 7, cy + 4);
        }
        break;

    case ICON_TYPE_CHESS:
        solar_os_gfx_circle(gfx, cx, cy - 4, 4);
        solar_os_gfx_fill_rect(gfx, cx - 3, cy, 6, 8);
        solar_os_gfx_fill_rect(gfx, cx - 6, cy + 8, 12, 3);
        solar_os_gfx_line(gfx, cx, cy - 10, cx, cy - 7);
        solar_os_gfx_line(gfx, cx - 2, cy - 9, cx + 2, cy - 9);
        break;

    case ICON_TYPE_GO:
        solar_os_gfx_rect(gfx, cx - 11, cy - 11, 22, 22);
        solar_os_gfx_line(gfx, cx, cy - 11, cx, cy + 11);
        solar_os_gfx_line(gfx, cx - 11, cy, cx + 11, cy);
        solar_os_gfx_fill_circle(gfx, cx - 4, cy - 4, 4);
        solar_os_gfx_circle(gfx, cx + 4, cy + 4, 4);
        break;

    case ICON_TYPE_SUDOKU:
        solar_os_gfx_rect(gfx, cx - 11, cy - 11, 22, 22);
        solar_os_gfx_line(gfx, cx - 4, cy - 11, cx - 4, cy + 11);
        solar_os_gfx_line(gfx, cx + 3, cy - 11, cx + 3, cy + 11);
        solar_os_gfx_line(gfx, cx - 11, cy - 4, cx + 11, cy - 4);
        solar_os_gfx_line(gfx, cx - 11, cy + 3, cx + 11, cy + 3);
        solar_os_gfx_fill_rect(gfx, cx - 3, cy - 3, 6, 6);
        break;

    case ICON_TYPE_WEBRADIO:
        solar_os_gfx_rect(gfx, cx - 11, cy - 6, 22, 16);
        solar_os_gfx_circle(gfx, cx - 4, cy + 2, 4);
        solar_os_gfx_line(gfx, cx - 10, cy - 13, cx - 4, cy - 6);
        solar_os_gfx_fill_circle(gfx, cx + 4, cy - 1, 2);
        solar_os_gfx_fill_circle(gfx, cx + 4, cy + 4, 2);
        break;

    case ICON_TYPE_SETTINGS_CP:
        solar_os_gfx_circle(gfx, cx, cy, 10);
        solar_os_gfx_circle(gfx, cx, cy, 6);
        solar_os_gfx_fill_circle(gfx, cx, cy, 3);
        solar_os_gfx_line(gfx, cx, cy - 12, cx, cy + 12);
        solar_os_gfx_line(gfx, cx - 12, cy, cx + 12, cy);
        break;

    case ICON_TYPE_FILE_SERVER:
        solar_os_gfx_rect(gfx, cx - 11, cy - 10, 22, 20);
        solar_os_gfx_line(gfx, cx - 11, cy, cx + 11, cy);
        solar_os_gfx_fill_circle(gfx, cx + 6, cy - 5, 2);
        solar_os_gfx_fill_circle(gfx, cx + 6, cy + 5, 2);
        solar_os_gfx_line(gfx, cx - 7, cy - 5, cx + 1, cy - 5);
        solar_os_gfx_line(gfx, cx - 7, cy + 5, cx + 1, cy + 5);
        break;

    case ICON_TYPE_THERMOMETER:
        solar_os_gfx_rect(gfx, cx - 3, cy - 12, 6, 18);
        solar_os_gfx_fill_circle(gfx, cx, cy + 8, 5);
        solar_os_gfx_fill_rect(gfx, cx - 2, cy - 2, 4, 8);
        solar_os_gfx_line(gfx, cx + 5, cy - 8, cx + 8, cy - 8);
        solar_os_gfx_line(gfx, cx + 5, cy - 3, cx + 8, cy - 3);
        solar_os_gfx_line(gfx, cx + 5, cy + 2, cx + 8, cy + 2);
        break;

    case ICON_TYPE_WEATHER:
        solar_os_gfx_circle(gfx, cx + 4, cy - 4, 6);
        solar_os_gfx_fill_circle(gfx, cx - 4, cy + 2, 5);
        solar_os_gfx_fill_circle(gfx, cx + 4, cy + 1, 6);
        solar_os_gfx_fill_rect(gfx, cx - 6, cy + 2, 14, 5);
        break;

    case ICON_TYPE_POMODORO:
        solar_os_gfx_circle(gfx, cx, cy + 1, 11);
        solar_os_gfx_circle(gfx, cx, cy + 1, 9);
        solar_os_gfx_line(gfx, cx, cy - 10, cx + 4, cy - 14);
        solar_os_gfx_line(gfx, cx, cy + 1, cx, cy - 5);
        solar_os_gfx_line(gfx, cx, cy + 1, cx + 4, cy + 1);
        break;

    case ICON_TYPE_PHOTOS:
        solar_os_gfx_rect(gfx, cx - 11, cy - 9, 22, 18);
        solar_os_gfx_circle(gfx, cx + 4, cy - 3, 3);
        solar_os_gfx_line(gfx, cx - 9, cy + 7, cx - 3, cy);
        solar_os_gfx_line(gfx, cx - 3, cy, cx + 3, cy + 5);
        solar_os_gfx_line(gfx, cx + 3, cy + 5, cx + 9, cy - 1);
        break;

    case ICON_TYPE_CALENDAR:
        solar_os_gfx_rect(gfx, cx - 11, cy - 10, 22, 20);
        solar_os_gfx_fill_rect(gfx, cx - 11, cy - 10, 22, 5);
        solar_os_gfx_line(gfx, cx - 6, cy - 12, cx - 6, cy - 9);
        solar_os_gfx_line(gfx, cx + 6, cy - 12, cx + 6, cy - 9);
        solar_os_gfx_fill_rect(gfx, cx - 6, cy - 1, 3, 3);
        solar_os_gfx_fill_rect(gfx, cx, cy - 1, 3, 3);
        solar_os_gfx_fill_rect(gfx, cx + 6, cy - 1, 3, 3);
        solar_os_gfx_fill_rect(gfx, cx - 6, cy + 4, 3, 3);
        solar_os_gfx_fill_rect(gfx, cx, cy + 4, 3, 3);
        break;

    case ICON_TYPE_GAMEBOY:
        solar_os_gfx_rect(gfx, cx - 11, cy - 14, 22, 28);
        solar_os_gfx_fill_rect(gfx, cx - 11, cy - 14, 2, 2);
        solar_os_gfx_fill_rect(gfx, cx + 9, cy - 14, 2, 2);
        solar_os_gfx_rect(gfx, cx - 8, cy - 11, 16, 12);
        solar_os_gfx_fill_rect(gfx, cx - 6, cy - 9, 12, 8);
        solar_os_gfx_set_color(gfx, bg);
        solar_os_gfx_fill_rect(gfx, cx - 4, cy - 7, 8, 4);
        solar_os_gfx_set_color(gfx, fg);
        solar_os_gfx_fill_rect(gfx, cx - 8, cy + 5, 6, 2);
        solar_os_gfx_fill_rect(gfx, cx - 6, cy + 3, 2, 6);
        solar_os_gfx_fill_circle(gfx, cx + 3, cy + 7, 2);
        solar_os_gfx_fill_circle(gfx, cx + 7, cy + 4, 2);
        break;

    case ICON_TYPE_INVADERS:
        solar_os_gfx_fill_rect(gfx, cx - 8, cy - 6, 16, 10);
        solar_os_gfx_fill_rect(gfx, cx - 11, cy - 2, 22, 4);
        solar_os_gfx_fill_rect(gfx, cx - 6, cy - 10, 3, 4);
        solar_os_gfx_fill_rect(gfx, cx + 3, cy - 10, 3, 4);
        solar_os_gfx_fill_rect(gfx, cx - 9, cy + 4, 4, 6);
        solar_os_gfx_fill_rect(gfx, cx + 5, cy + 4, 4, 6);
        solar_os_gfx_set_color(gfx, bg);
        solar_os_gfx_fill_rect(gfx, cx - 5, cy - 4, 3, 3);
        solar_os_gfx_fill_rect(gfx, cx + 2, cy - 4, 3, 3);
        solar_os_gfx_set_color(gfx, fg);
        break;

    case ICON_TYPE_FILES:
        solar_os_gfx_rect(gfx, cx - 12, cy - 8, 24, 18);
        solar_os_gfx_fill_rect(gfx, cx - 12, cy - 12, 10, 4);
        solar_os_gfx_line(gfx, cx - 12, cy - 2, cx + 11, cy - 2);
        solar_os_gfx_fill_rect(gfx, cx - 8, cy + 2, 6, 4);
        break;

    case ICON_TYPE_CALC:
        solar_os_gfx_rect(gfx, cx - 10, cy - 13, 20, 26);
        solar_os_gfx_fill_rect(gfx, cx - 7, cy - 10, 14, 5);
        solar_os_gfx_fill_rect(gfx, cx - 7, cy - 2, 5, 4);
        solar_os_gfx_fill_rect(gfx, cx + 2, cy - 2, 5, 4);
        solar_os_gfx_fill_rect(gfx, cx - 7, cy + 4, 5, 4);
        solar_os_gfx_fill_rect(gfx, cx + 2, cy + 4, 5, 4);
        break;

    case ICON_TYPE_WRITER:
        solar_os_gfx_rect(gfx, cx - 9, cy - 13, 18, 26);
        solar_os_gfx_line(gfx, cx - 6, cy - 8, cx + 6, cy - 8);
        solar_os_gfx_line(gfx, cx - 6, cy - 4, cx + 6, cy - 4);
        solar_os_gfx_line(gfx, cx - 6, cy, cx + 6, cy);
        solar_os_gfx_fill_rect(gfx, cx + 4, cy + 6, 6, 6);
        break;

    case ICON_TYPE_READER:
        solar_os_gfx_rect(gfx, cx - 12, cy - 8, 12, 18);
        solar_os_gfx_rect(gfx, cx, cy - 8, 12, 18);
        solar_os_gfx_line(gfx, cx - 9, cy - 4, cx - 3, cy - 4);
        solar_os_gfx_line(gfx, cx + 3, cy - 4, cx + 9, cy - 4);
        break;

    case ICON_TYPE_PLAYER:
        solar_os_gfx_circle(gfx, cx, cy, 12);
        solar_os_gfx_fill_circle(gfx, cx - 5, cy + 4, 3);
        solar_os_gfx_fill_circle(gfx, cx + 5, cy + 2, 3);
        solar_os_gfx_line(gfx, cx - 2, cy + 4, cx - 2, cy - 6);
        solar_os_gfx_line(gfx, cx + 8, cy + 2, cx + 8, cy - 8);
        solar_os_gfx_fill_rect(gfx, cx - 2, cy - 8, 11, 4);
        break;

    case ICON_TYPE_CLOCK:
        solar_os_gfx_circle(gfx, cx, cy, 12);
        solar_os_gfx_circle(gfx, cx, cy, 11);
        solar_os_gfx_line(gfx, cx, cy, cx, cy - 7);
        solar_os_gfx_line(gfx, cx, cy, cx + 5, cy);
        solar_os_gfx_fill_circle(gfx, cx, cy, 2);
        break;

    case ICON_TYPE_PYTHON:
        solar_os_gfx_rect(gfx, cx - 11, cy - 11, 22, 22);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, cx - 8, cy + 4, "PY");
        break;

    case ICON_TYPE_LUA:
        solar_os_gfx_circle(gfx, cx, cy, 11);
        solar_os_gfx_fill_circle(gfx, cx + 4, cy - 4, 3);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, cx - 8, cy + 5, "LU");
        break;

    case ICON_TYPE_BLE:
        solar_os_gfx_circle(gfx, cx, cy, 12);
        solar_os_gfx_line(gfx, cx, cy - 9, cx, cy + 9);
        solar_os_gfx_line(gfx, cx, cy - 9, cx + 5, cy - 4);
        solar_os_gfx_line(gfx, cx + 5, cy - 4, cx - 4, cy + 4);
        solar_os_gfx_line(gfx, cx - 4, cy - 4, cx + 5, cy + 4);
        solar_os_gfx_line(gfx, cx + 5, cy + 4, cx, cy + 9);
        break;

    case ICON_TYPE_SHELL:
        solar_os_gfx_rect(gfx, cx - 11, cy - 10, 22, 20);
        solar_os_gfx_line(gfx, cx - 7, cy - 5, cx - 4, cy - 2);
        solar_os_gfx_line(gfx, cx - 4, cy - 2, cx - 7, cy + 1);
        solar_os_gfx_line(gfx, cx - 2, cy + 3, cx + 4, cy + 3);
        break;

    case ICON_TYPE_REBOOT:
        solar_os_gfx_circle(gfx, cx, cy, 10);
        solar_os_gfx_line(gfx, cx, cy - 10, cx + 4, cy - 6);
        solar_os_gfx_line(gfx, cx, cy - 10, cx + 4, cy - 14);
        solar_os_gfx_fill_circle(gfx, cx, cy, 3);
        break;

    case ICON_TYPE_CHAT:
        solar_os_gfx_rect(gfx, cx - 11, cy - 10, 16, 12);
        solar_os_gfx_fill_rect(gfx, cx - 9, cy + 2, 4, 4);
        solar_os_gfx_rect(gfx, cx - 3, cy - 3, 14, 11);
        solar_os_gfx_fill_rect(gfx, cx + 7, cy + 8, 3, 3);
        break;

    case ICON_TYPE_WEB:
        solar_os_gfx_circle(gfx, cx, cy, 12);
        solar_os_gfx_line(gfx, cx - 12, cy, cx + 12, cy);
        solar_os_gfx_circle(gfx, cx, cy, 6);
        break;

    case ICON_TYPE_AGENT:
        solar_os_gfx_rect(gfx, cx - 10, cy - 8, 20, 18);
        solar_os_gfx_fill_rect(gfx, cx - 7, cy - 4, 4, 4);
        solar_os_gfx_fill_rect(gfx, cx + 3, cy - 4, 4, 4);
        solar_os_gfx_line(gfx, cx - 5, cy + 4, cx + 5, cy + 4);
        solar_os_gfx_line(gfx, cx, cy - 8, cx, cy - 12);
        solar_os_gfx_fill_circle(gfx, cx, cy - 13, 2);
        break;

    case ICON_TYPE_WIFI_SETUP:
        solar_os_gfx_circle(gfx, cx, cy + 7, 2);
        solar_os_gfx_fill_circle(gfx, cx, cy + 7, 2);
        solar_os_gfx_circle(gfx, cx, cy + 7, 6);
        solar_os_gfx_circle(gfx, cx, cy + 7, 11);
        break;

    case ICON_TYPE_RECORDER:
        /* Microphone body */
        solar_os_gfx_fill_rect(gfx, cx - 4, cy - 10, 8, 12);
        solar_os_gfx_circle(gfx, cx, cy - 2, 7);
        solar_os_gfx_line(gfx, cx, cy + 5, cx, cy + 10);
        solar_os_gfx_line(gfx, cx - 6, cy + 10, cx + 6, cy + 10);
        break;

    case ICON_TYPE_STOPWATCH:
        /* Stopwatch with top pushers */
        solar_os_gfx_circle(gfx, cx, cy + 2, 10);
        solar_os_gfx_circle(gfx, cx, cy + 2, 9);
        solar_os_gfx_fill_rect(gfx, cx - 2, cy - 11, 4, 3);
        solar_os_gfx_line(gfx, cx + 6, cy - 7, cx + 9, cy - 10);
        solar_os_gfx_line(gfx, cx, cy + 2, cx, cy - 4);
        solar_os_gfx_line(gfx, cx, cy + 2, cx + 4, cy + 2);
        solar_os_gfx_fill_circle(gfx, cx, cy + 2, 2);
        break;

    case ICON_TYPE_TIMER:
        /* Hourglass / Timer */
        solar_os_gfx_line(gfx, cx - 8, cy - 10, cx + 8, cy - 10);
        solar_os_gfx_line(gfx, cx - 8, cy + 10, cx + 8, cy + 10);
        solar_os_gfx_line(gfx, cx - 7, cy - 9, cx + 7, cy + 9);
        solar_os_gfx_line(gfx, cx + 7, cy - 9, cx - 7, cy + 9);
        solar_os_gfx_fill_circle(gfx, cx, cy - 4, 2);
        solar_os_gfx_fill_circle(gfx, cx, cy + 5, 3);
        break;

    case ICON_TYPE_KEYBOARD_LAYOUT:
        /* Keyboard icon: rectangular frame with key grid */
        solar_os_gfx_rect(gfx, cx - 13, cy - 8, 26, 17);
        solar_os_gfx_fill_rect(gfx, cx - 10, cy - 5, 4, 3);
        solar_os_gfx_fill_rect(gfx, cx - 4, cy - 5, 4, 3);
        solar_os_gfx_fill_rect(gfx, cx + 2, cy - 5, 4, 3);
        solar_os_gfx_fill_rect(gfx, cx + 8, cy - 5, 4, 3);
        solar_os_gfx_fill_rect(gfx, cx - 10, cy, 4, 3);
        solar_os_gfx_fill_rect(gfx, cx - 4, cy, 4, 3);
        solar_os_gfx_fill_rect(gfx, cx + 2, cy, 4, 3);
        solar_os_gfx_fill_rect(gfx, cx + 8, cy, 4, 3);
        solar_os_gfx_fill_rect(gfx, cx - 6, cy + 4, 12, 3);
        break;

    default:
        solar_os_gfx_rect(gfx, cx - 10, cy - 10, 20, 20);
        solar_os_gfx_fill_circle(gfx, cx, cy, 3);
        break;
    }
}

/* ----------------- 2-Line Text Wrapping Helper ----------------- */

static void draw_wrapped_card_label(solar_os_gfx_t *gfx, int card_x, int card_y, int card_w, const char *label, bool is_selected)
{
    solar_os_gfx_set_color(gfx, is_selected ? SOLAR_OS_GFX_COLOR_WHITE : SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    char line1[24] = "";
    char line2[24] = "";

    const char *space = strchr(label, ' ');
    if (space != NULL && (size_t)(space - label) < sizeof(line1)) {
        size_t l1_len = (size_t)(space - label);
        memcpy(line1, label, l1_len);
        line1[l1_len] = '\0';
        strlcpy(line2, space + 1, sizeof(line2));

        const size_t w1 = solar_os_gfx_text_width(gfx, line1);
        const size_t w2 = solar_os_gfx_text_width(gfx, line2);

        solar_os_gfx_text(gfx, card_x + (card_w - (int)w1) / 2, card_y + 70, line1);
        solar_os_gfx_text(gfx, card_x + (card_w - (int)w2) / 2, card_y + 83, line2);
    } else {
        const size_t w = solar_os_gfx_text_width(gfx, label);
        solar_os_gfx_text(gfx, card_x + (card_w - (int)w) / 2, card_y + 76, label);
    }
}

static void truncate_filename(const char *src, char *dst, size_t max_len)
{
    if (src == NULL || dst == NULL || max_len == 0) return;
    const size_t len = strlen(src);
    if (len <= max_len) {
        strlcpy(dst, src, max_len + 1);
        return;
    }

    const char *ext = strrchr(src, '.');
    const size_t ext_len = ext != NULL ? strlen(ext) : 0;
    if (max_len > ext_len + 4) {
        const size_t prefix_len = max_len - ext_len - 2;
        memcpy(dst, src, prefix_len);
        dst[prefix_len] = '\0';
        strlcat(dst, "..", max_len + 1);
        if (ext != NULL) {
            strlcat(dst, ext, max_len + 1);
        }
    } else {
        memcpy(dst, src, max_len - 2);
        dst[max_len - 2] = '\0';
        strlcat(dst, "..", max_len + 1);
    }
}

static void add_folder_item(size_t folder_idx, launcher_item_kind_t kind,
                            const char *app_or_path, const char *display_label,
                            const char *desc, launcher_icon_type_t icon_type)
{
    if (folder_idx >= LAUNCHER_FOLDER_COUNT) return;
    launcher_folder_t *f = &launcher.folders[folder_idx];
    if (f->count >= LAUNCHER_MAX_ITEMS_PER_FOLDER) return;

    if (kind == ITEM_KIND_BUILTIN && solar_os_app_registry_find(app_or_path) == NULL) {
        return;
    }

    launcher_item_t *item = &f->items[f->count];
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->icon_type = icon_type;
    strlcpy(item->name, app_or_path, sizeof(item->name));
    strlcpy(item->display_label, display_label, sizeof(item->display_label));
    strlcpy(item->description, desc, sizeof(item->description));
    if (kind == ITEM_KIND_PYTHON || kind == ITEM_KIND_LUA || kind == ITEM_KIND_GAMEBOY_ROM) {
        strlcpy(item->path, app_or_path, sizeof(item->path));
    }
    f->count++;
}

static void ensure_dir_exists(const char *path)
{
    char tmp[128];
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static const char *create_default_file_for_app(const char *app_name)
{
    static char target_path[128];
    const char *mount = solar_os_storage_sd_is_mounted() ?
        solar_os_storage_sd_mount_point() :
        (solar_os_storage_flash_is_mounted() ? solar_os_storage_flash_mount_point() : "/sdcard");

    if (strcmp(app_name, "writer") == 0) {
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/doc", mount);
        ensure_dir_exists(dir);
        snprintf(target_path, sizeof(target_path), "%s/new_document.md", dir);
        FILE *f = fopen(target_path, "r");
        if (f == NULL) {
            f = fopen(target_path, "w");
            if (f != NULL) {
                fputs("# New Document\n\nStart typing markdown here...\n", f);
                fclose(f);
            }
        } else {
            fclose(f);
        }
    } else if (strcmp(app_name, "edit") == 0) {
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/scripts", mount);
        ensure_dir_exists(dir);
        snprintf(target_path, sizeof(target_path), "%s/new_script.py", dir);
        FILE *f = fopen(target_path, "r");
        if (f == NULL) {
            f = fopen(target_path, "w");
            if (f != NULL) {
                fputs("# SolarOS Python Script\n\nprint('Hello from SolarOS!')\n", f);
                fclose(f);
            }
        } else {
            fclose(f);
        }
    } else if (strcmp(app_name, "sheet") == 0) {
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/doc", mount);
        ensure_dir_exists(dir);
        snprintf(target_path, sizeof(target_path), "%s/new_sheet.csv", dir);
        FILE *f = fopen(target_path, "r");
        if (f == NULL) {
            f = fopen(target_path, "w");
            if (f != NULL) {
                fputs("Item,Quantity,Unit_Price,Total\nApples,10,1.50,15.00\nOranges,8,2.00,16.00\nBananas,12,0.80,9.60\n", f);
                fclose(f);
            }
        } else {
            fclose(f);
        }
    } else if (strcmp(app_name, "notes") == 0) {
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/notes", mount);
        ensure_dir_exists(dir);
        snprintf(target_path, sizeof(target_path), "%s/quick_note.md", dir);
        FILE *f = fopen(target_path, "r");
        if (f == NULL) {
            f = fopen(target_path, "w");
            if (f != NULL) {
                fputs("# Quick Notes\n\n- Task 1: Check system\n- Task 2: Write notes\n", f);
                fclose(f);
            }
        } else {
            fclose(f);
        }
    } else {
        snprintf(target_path, sizeof(target_path), "%s/default.txt", mount);
        FILE *f = fopen(target_path, "a");
        if (f != NULL) fclose(f);
    }

    return target_path;
}

static bool is_file_picker_app(const char *app_name)
{
    if (app_name == NULL) return false;
    return (strcmp(app_name, "writer") == 0 ||
            strcmp(app_name, "reader") == 0 ||
            strcmp(app_name, "notes") == 0 ||
            strcmp(app_name, "edit") == 0 ||
            strcmp(app_name, "sheet") == 0 ||
            strcmp(app_name, "player") == 0 ||
            strcmp(app_name, "view") == 0 ||
            strcmp(app_name, "gameboy") == 0);
}

static void scan_dir_for_extensions(const char *dir_path, const char *const *exts, size_t ext_count, launcher_icon_type_t icon_type)
{
    if (dir_path == NULL) return;
    DIR *dir = opendir(dir_path);
    if (dir == NULL) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        const char *dot = strrchr(entry->d_name, '.');
        if (dot == NULL) continue;

        for (size_t i = 0; i < ext_count; i++) {
            if (strcasecmp(dot, exts[i]) == 0) {
                if (launcher.picker_count < 64) {
                    launcher_picker_entry_t *p = &launcher.picker_items[launcher.picker_count];
                    snprintf(p->path, sizeof(p->path), "%s/%s", dir_path, entry->d_name);
                    truncate_filename(entry->d_name, p->name, 28);
                    p->icon_type = icon_type;
                    p->is_new_file = false;
                    launcher.picker_count++;
                }
                break;
            }
        }
    }
    closedir(dir);
}

static void launcher_open_file_picker_for_app(const char *app_name)
{
    launcher.picker_count = 0;
    launcher.selected_picker_item = 0;
    strlcpy(launcher.picker_app_name, app_name, sizeof(launcher.picker_app_name));

    const char *sd = solar_os_storage_sd_is_mounted() ? solar_os_storage_sd_mount_point() : NULL;
    const char *flash = solar_os_storage_flash_is_mounted() ? solar_os_storage_flash_mount_point() : NULL;

    if (strcmp(app_name, "gameboy") == 0) {
        strlcpy(launcher.picker_title, "GAME BOY - SELECT ROM", sizeof(launcher.picker_title));
        launcher.picker_icon = ICON_TYPE_GAMEBOY;
        const char *exts[] = {".gb", ".gbc"};
        if (sd) {
            char p[64];
            snprintf(p, sizeof(p), "%s/roms", sd); scan_dir_for_extensions(p, exts, 2, ICON_TYPE_GAMEBOY);
            snprintf(p, sizeof(p), "%s/games", sd); scan_dir_for_extensions(p, exts, 2, ICON_TYPE_GAMEBOY);
            scan_dir_for_extensions(sd, exts, 2, ICON_TYPE_GAMEBOY);
        }
        if (flash) {
            char p[64];
            snprintf(p, sizeof(p), "%s/roms", flash); scan_dir_for_extensions(p, exts, 2, ICON_TYPE_GAMEBOY);
            scan_dir_for_extensions(flash, exts, 2, ICON_TYPE_GAMEBOY);
        }
    } else if (strcmp(app_name, "writer") == 0) {
        strlcpy(launcher.picker_title, "TYPEWRITER - SELECT DOCUMENT", sizeof(launcher.picker_title));
        launcher.picker_icon = ICON_TYPE_WRITER;
        launcher_picker_entry_t *p = &launcher.picker_items[launcher.picker_count++];
        strlcpy(p->name, "[ + Create New Document ]", sizeof(p->name));
        p->path[0] = '\0';
        p->icon_type = ICON_TYPE_WRITER;
        p->is_new_file = true;

        const char *exts[] = {".md", ".txt"};
        if (sd) {
            char path[64];
            snprintf(path, sizeof(path), "%s/doc", sd); scan_dir_for_extensions(path, exts, 2, ICON_TYPE_WRITER);
            snprintf(path, sizeof(path), "%s/docs", sd); scan_dir_for_extensions(path, exts, 2, ICON_TYPE_WRITER);
            scan_dir_for_extensions(sd, exts, 2, ICON_TYPE_WRITER);
        }
        if (flash) {
            scan_dir_for_extensions(flash, exts, 2, ICON_TYPE_WRITER);
        }
    } else if (strcmp(app_name, "reader") == 0) {
        strlcpy(launcher.picker_title, "E-BOOK READER - SELECT BOOK", sizeof(launcher.picker_title));
        launcher.picker_icon = ICON_TYPE_READER;
        const char *exts[] = {".epub", ".md", ".txt"};
        if (sd) {
            char path[64];
            snprintf(path, sizeof(path), "%s/doc", sd); scan_dir_for_extensions(path, exts, 3, ICON_TYPE_READER);
            snprintf(path, sizeof(path), "%s/books", sd); scan_dir_for_extensions(path, exts, 3, ICON_TYPE_READER);
            scan_dir_for_extensions(sd, exts, 3, ICON_TYPE_READER);
        }
        if (flash) scan_dir_for_extensions(flash, exts, 3, ICON_TYPE_READER);
    } else if (strcmp(app_name, "notes") == 0) {
        strlcpy(launcher.picker_title, "QUICK NOTES - SELECT NOTE", sizeof(launcher.picker_title));
        launcher.picker_icon = ICON_TYPE_NOTES;
        launcher_picker_entry_t *p = &launcher.picker_items[launcher.picker_count++];
        strlcpy(p->name, "[ + Create New Note ]", sizeof(p->name));
        p->path[0] = '\0';
        p->icon_type = ICON_TYPE_NOTES;
        p->is_new_file = true;

        const char *exts[] = {".txt", ".note", ".md"};
        if (sd) {
            char path[64];
            snprintf(path, sizeof(path), "%s/notes", sd); scan_dir_for_extensions(path, exts, 3, ICON_TYPE_NOTES);
            snprintf(path, sizeof(path), "%s/doc", sd); scan_dir_for_extensions(path, exts, 3, ICON_TYPE_NOTES);
            scan_dir_for_extensions(sd, exts, 3, ICON_TYPE_NOTES);
        }
        if (flash) scan_dir_for_extensions(flash, exts, 3, ICON_TYPE_NOTES);
    } else if (strcmp(app_name, "edit") == 0) {
        strlcpy(launcher.picker_title, "CODE EDITOR - SELECT FILE", sizeof(launcher.picker_title));
        launcher.picker_icon = ICON_TYPE_EDIT;
        launcher_picker_entry_t *p = &launcher.picker_items[launcher.picker_count++];
        strlcpy(p->name, "[ + Create New File ]", sizeof(p->name));
        p->path[0] = '\0';
        p->icon_type = ICON_TYPE_EDIT;
        p->is_new_file = true;

        const char *exts[] = {".py", ".lua", ".c", ".h", ".json", ".txt", ".md", ".csv"};
        if (sd) {
            char path[64];
            snprintf(path, sizeof(path), "%s/scripts", sd); scan_dir_for_extensions(path, exts, 8, ICON_TYPE_EDIT);
            scan_dir_for_extensions(sd, exts, 8, ICON_TYPE_EDIT);
        }
        if (flash) scan_dir_for_extensions(flash, exts, 8, ICON_TYPE_EDIT);
    } else if (strcmp(app_name, "sheet") == 0) {
        strlcpy(launcher.picker_title, "SPREADSHEET - SELECT CSV", sizeof(launcher.picker_title));
        launcher.picker_icon = ICON_TYPE_SHEET;
        launcher_picker_entry_t *p = &launcher.picker_items[launcher.picker_count++];
        strlcpy(p->name, "[ + Create New Sheet ]", sizeof(p->name));
        p->path[0] = '\0';
        p->icon_type = ICON_TYPE_SHEET;
        p->is_new_file = true;

        const char *exts[] = {".csv"};
        if (sd) {
            char path[64];
            snprintf(path, sizeof(path), "%s/doc", sd); scan_dir_for_extensions(path, exts, 1, ICON_TYPE_SHEET);
            scan_dir_for_extensions(sd, exts, 1, ICON_TYPE_SHEET);
        }
        if (flash) scan_dir_for_extensions(flash, exts, 1, ICON_TYPE_SHEET);
    } else if (strcmp(app_name, "player") == 0) {
        strlcpy(launcher.picker_title, "AUDIO PLAYER - SELECT TRACK", sizeof(launcher.picker_title));
        launcher.picker_icon = ICON_TYPE_PLAYER;
        const char *exts[] = {".mp3", ".wav"};
        if (sd) {
            char path[64];
            snprintf(path, sizeof(path), "%s/music", sd); scan_dir_for_extensions(path, exts, 2, ICON_TYPE_PLAYER);
            scan_dir_for_extensions(sd, exts, 2, ICON_TYPE_PLAYER);
        }
        if (flash) scan_dir_for_extensions(flash, exts, 2, ICON_TYPE_PLAYER);
    } else if (strcmp(app_name, "view") == 0) {
        strlcpy(launcher.picker_title, "PHOTO GALLERY - SELECT IMAGE", sizeof(launcher.picker_title));
        launcher.picker_icon = ICON_TYPE_VIEW;
        const char *exts[] = {".bmp", ".png", ".jpg", ".jpeg", ".webp", ".pbm"};
        if (sd) {
            char path[64];
            snprintf(path, sizeof(path), "%s/screencapture", sd); scan_dir_for_extensions(path, exts, 6, ICON_TYPE_VIEW);
            snprintf(path, sizeof(path), "%s/screenshots", sd); scan_dir_for_extensions(path, exts, 6, ICON_TYPE_VIEW);
            snprintf(path, sizeof(path), "%s/screenshot", sd); scan_dir_for_extensions(path, exts, 6, ICON_TYPE_VIEW);
            snprintf(path, sizeof(path), "%s/photos", sd); scan_dir_for_extensions(path, exts, 6, ICON_TYPE_VIEW);
            snprintf(path, sizeof(path), "%s/images", sd); scan_dir_for_extensions(path, exts, 6, ICON_TYPE_VIEW);
            scan_dir_for_extensions(sd, exts, 6, ICON_TYPE_VIEW);
        }
        if (flash) {
            char path[64];
            snprintf(path, sizeof(path), "%s/screencapture", flash); scan_dir_for_extensions(path, exts, 6, ICON_TYPE_VIEW);
            snprintf(path, sizeof(path), "%s/photos", flash); scan_dir_for_extensions(path, exts, 6, ICON_TYPE_VIEW);
            snprintf(path, sizeof(path), "%s/images", flash); scan_dir_for_extensions(path, exts, 6, ICON_TYPE_VIEW);
            scan_dir_for_extensions(flash, exts, 6, ICON_TYPE_VIEW);
        }
    }

    launcher.view_mode = VIEW_ROM_PICKER;
}

static void scan_directory_for_scripts(const char *dir_path)
{
    if (dir_path == NULL) return;
    DIR *dir = opendir(dir_path);
    if (dir == NULL) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        const char *dot = strrchr(entry->d_name, '.');
        if (dot == NULL) continue;

        char full_path[LAUNCHER_MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        char short_name[LAUNCHER_MAX_LABEL];
        truncate_filename(entry->d_name, short_name, 12);

        if (strcasecmp(dot, ".py") == 0) {
            char desc[LAUNCHER_MAX_DESC];
            snprintf(desc, sizeof(desc), "Python script: %s", entry->d_name);
            add_folder_item(4, ITEM_KIND_PYTHON, full_path, short_name, desc, ICON_TYPE_PYTHON);
        } else if (strcasecmp(dot, ".lua") == 0) {
            char desc[LAUNCHER_MAX_DESC];
            snprintf(desc, sizeof(desc), "Lua script: %s", entry->d_name);
            add_folder_item(4, ITEM_KIND_LUA, full_path, short_name, desc, ICON_TYPE_LUA);
        }
    }
    closedir(dir);
}

static void launcher_refresh_items(void)
{
    /* Folder 0: System */
    strlcpy(launcher.folders[0].title, "System", sizeof(launcher.folders[0].title));
    strlcpy(launcher.folders[0].description, "Files, calculator, charts, thermometer and utilities", sizeof(launcher.folders[0].description));
    launcher.folders[0].icon_type = ICON_TYPE_FOLDER_SYSTEM;
    launcher.folders[0].count = 0;

    add_folder_item(0, ITEM_KIND_BUILTIN, "files", "Files", "Dual-pane graphical file manager", ICON_TYPE_FILES);
    add_folder_item(0, ITEM_KIND_BUILTIN, "calc", "Calculator", "Scientific and financial calculator", ICON_TYPE_CALC);
    add_folder_item(0, ITEM_KIND_BUILTIN, "sheet", "Spreadsheet", "Multi-cell CSV sheet viewer", ICON_TYPE_SHEET);
    add_folder_item(0, ITEM_KIND_BUILTIN, "plot", "Graph Plotter", "Mathematical function plotter", ICON_TYPE_PLOT);
    add_folder_item(0, ITEM_KIND_BUILTIN, "logic", "Logic Analyzer", "Hardware digital signal analyzer", ICON_TYPE_LOGIC);
    add_folder_item(0, ITEM_KIND_BUILTIN, "thermometer", "Thermometer", "Internal hardware temperature sensor", ICON_TYPE_THERMOMETER);
    add_folder_item(0, ITEM_KIND_BUILTIN, "calendar", "Calendar", "Monthly calendar & live clock", ICON_TYPE_CALENDAR);
    add_folder_item(0, ITEM_KIND_BUILTIN, "clock", "World Clock", "Clock and multi-timezone viewer", ICON_TYPE_CLOCK);
    add_folder_item(0, ITEM_KIND_BUILTIN, "stopwatch", "Stopwatch", "Precision stopwatch with lap splits", ICON_TYPE_STOPWATCH);
    add_folder_item(0, ITEM_KIND_BUILTIN, "timer", "Timer", "Countdown timer with audible alarm", ICON_TYPE_TIMER);
    add_folder_item(0, ITEM_KIND_BUILTIN, "keytest", "Key Test", "Real-time key inspector & layout tester", ICON_TYPE_KEYBOARD_LAYOUT);

    /* Folder 1: Text & Reading */
    strlcpy(launcher.folders[1].title, "Text & Reading", sizeof(launcher.folders[1].title));
    strlcpy(launcher.folders[1].description, "Typewriter, book reader, notes and text editor", sizeof(launcher.folders[1].description));
    launcher.folders[1].icon_type = ICON_TYPE_FOLDER_TEXT;
    launcher.folders[1].count = 0;

    add_folder_item(1, ITEM_KIND_BUILTIN, "writer", "Typewriter", "Distraction-free markdown writer", ICON_TYPE_WRITER);
    add_folder_item(1, ITEM_KIND_BUILTIN, "reader", "Book Reader", "EPUB, markdown and document reader", ICON_TYPE_READER);
    add_folder_item(1, ITEM_KIND_BUILTIN, "notes", "Quick Notes", "Rapid notepad application", ICON_TYPE_NOTES);
    add_folder_item(1, ITEM_KIND_BUILTIN, "edit", "Code Editor", "Syntax-highlighted code editor", ICON_TYPE_EDIT);
    add_folder_item(1, ITEM_KIND_BUILTIN, "docs", "SolarOS Docs", "System manual and documentation", ICON_TYPE_DOCS);

    /* Folder 2: Games & Media (Multi-page with 12 items) */
    strlcpy(launcher.folders[2].title, "Games & Media", sizeof(launcher.folders[2].title));
    strlcpy(launcher.folders[2].description, "Chess, Go, Sudoku, Game Boy, recorder, player and media", sizeof(launcher.folders[2].description));
    launcher.folders[2].icon_type = ICON_TYPE_FOLDER_MEDIA;
    launcher.folders[2].count = 0;

    add_folder_item(2, ITEM_KIND_GAMEBOY_LAUNCHER, "gameboy", "Game Boy", "Game Boy emulator & ROM picker", ICON_TYPE_GAMEBOY);
    add_folder_item(2, ITEM_KIND_BUILTIN, "chess", "Chess", "Classic 8x8 chessboard game", ICON_TYPE_CHESS);
    add_folder_item(2, ITEM_KIND_BUILTIN, "go", "Go Game", "Classic 9x9 board game of Go", ICON_TYPE_GO);
    add_folder_item(2, ITEM_KIND_BUILTIN, "sudoku", "Sudoku", "9x9 number puzzle challenge", ICON_TYPE_SUDOKU);
    add_folder_item(2, ITEM_KIND_BUILTIN, "invaders", "Space Invaders", "Classic arcade space shooter", ICON_TYPE_INVADERS);
    add_folder_item(2, ITEM_KIND_BUILTIN, "pomodoro", "Pomodoro", "Visual circular Pomodoro focus timer", ICON_TYPE_POMODORO);
    add_folder_item(2, ITEM_KIND_BUILTIN, "player", "Audio Player", "WAV & MP3 stereo audio player", ICON_TYPE_PLAYER);
    add_folder_item(2, ITEM_KIND_BUILTIN, "recorder", "Voice Recorder", "WAV voice recorder & VU meter", ICON_TYPE_RECORDER);
    add_folder_item(2, ITEM_KIND_BUILTIN, "photos", "Photo Frame", "SD card photo slideshow viewer", ICON_TYPE_PHOTOS);
    add_folder_item(2, ITEM_KIND_BUILTIN, "synth", "Synthesizer", "Polyphonic audio synthesizer", ICON_TYPE_SYNTH);
    add_folder_item(2, ITEM_KIND_BUILTIN, "webradio", "Web Radio", "Live streaming internet radio", ICON_TYPE_WEBRADIO);
    add_folder_item(2, ITEM_KIND_BUILTIN, "view", "Gallery", "Image and graphics viewer", ICON_TYPE_VIEW);

    /* Folder 3: Network & Comms */
    strlcpy(launcher.folders[3].title, "Network & Comms", sizeof(launcher.folders[3].title));
    strlcpy(launcher.folders[3].description, "Weather, file server, mesh chat, browser and terminal", sizeof(launcher.folders[3].description));
    launcher.folders[3].icon_type = ICON_TYPE_FOLDER_NETWORK;
    launcher.folders[3].count = 0;

    add_folder_item(3, ITEM_KIND_BUILTIN, "weather", "Weather", "7-day graphical online forecast", ICON_TYPE_WEATHER);
    add_folder_item(3, ITEM_KIND_BUILTIN, "web_files", "File Server", "Wi-Fi SD card HTTP web server", ICON_TYPE_FILE_SERVER);
    add_folder_item(3, ITEM_KIND_BUILTIN, "chat", "Mesh Chat", "Encrypted Link/MeshCore network chat", ICON_TYPE_CHAT);
    add_folder_item(3, ITEM_KIND_BUILTIN, "web", "Web Browser", "HTTP/HTTPS text web browser", ICON_TYPE_WEB);
    add_folder_item(3, ITEM_KIND_BUILTIN, "email", "Email Client", "IMAP email reader and client", ICON_TYPE_EMAIL);
    add_folder_item(3, ITEM_KIND_BUILTIN, "ssh", "SSH Shell", "Secure remote terminal client", ICON_TYPE_SSH);
    add_folder_item(3, ITEM_KIND_BUILTIN, "agent", "AI Agent", "Artificial intelligence assistant", ICON_TYPE_AGENT);

    /* Folder 4: Python & Lua */
    strlcpy(launcher.folders[4].title, "Python & Lua", sizeof(launcher.folders[4].title));
    strlcpy(launcher.folders[4].description, "SD card and flash runtime scripts", sizeof(launcher.folders[4].description));
    launcher.folders[4].icon_type = ICON_TYPE_FOLDER_SCRIPT;
    launcher.folders[4].count = 0;

    add_folder_item(4, ITEM_KIND_BUILTIN, "python", "Python REPL", "Interactive MicroPython shell", ICON_TYPE_PYTHON);
    add_folder_item(4, ITEM_KIND_BUILTIN, "lua", "Lua REPL", "Interactive Lua shell", ICON_TYPE_LUA);

    if (solar_os_storage_sd_is_mounted()) {
        const char *sd_root = solar_os_storage_sd_mount_point();
        char sd_scripts[64];
        snprintf(sd_scripts, sizeof(sd_scripts), "%s/scripts", sd_root);
        scan_directory_for_scripts(sd_root);
        scan_directory_for_scripts(sd_scripts);
    }
    if (solar_os_storage_flash_is_mounted()) {
        const char *flash_root = solar_os_storage_flash_mount_point();
        char flash_scripts[64];
        snprintf(flash_scripts, sizeof(flash_scripts), "%s/scripts", flash_root);
        scan_directory_for_scripts(flash_root);
        scan_directory_for_scripts(flash_scripts);
    }

    /* Folder 5: Settings & Tools */
    strlcpy(launcher.folders[5].title, "Settings & Tools", sizeof(launcher.folders[5].title));
    strlcpy(launcher.folders[5].description, "Control panel, Wi-Fi setup, file server and tools", sizeof(launcher.folders[5].description));
    launcher.folders[5].icon_type = ICON_TYPE_FOLDER_SETTINGS;
    launcher.folders[5].count = 0;

    add_folder_item(5, ITEM_KIND_BUILTIN, "settings_gui", "Control Panel", "Full system & app settings GUI", ICON_TYPE_SETTINGS_CP);
    add_folder_item(5, ITEM_KIND_BUILTIN, "wifi_setup", "Wi-Fi Setup", "Scan & connect to Wi-Fi networks", ICON_TYPE_WIFI_SETUP);
    add_folder_item(5, ITEM_KIND_BUILTIN, "web_files", "Web Server", "Wi-Fi SD Card HTTP file manager", ICON_TYPE_FILE_SERVER);
    add_folder_item(5, ITEM_KIND_ACTION_BLE_PAIR, "ble_pair", "BLE Pairing", "Start keyboard pairing (PIN: 123456)", ICON_TYPE_BLE);
    add_folder_item(5, ITEM_KIND_ACTION_KEYBOARD_LAYOUT, "kbd_layout", "Keyboard Layout", "Switch between US, TR (Turkish Q), DE", ICON_TYPE_KEYBOARD_LAYOUT);
    add_folder_item(5, ITEM_KIND_ACTION_SHELL, "shell", "CLI Terminal", "Exit launcher to SolarOS shell", ICON_TYPE_SHELL);
    add_folder_item(5, ITEM_KIND_ACTION_REBOOT, "reboot", "Restart Device", "Reboot ESP32-S3 module", ICON_TYPE_REBOOT);
}

/* ----------------- Screensaver Dashboard Renderer ----------------- */

static void draw_screensaver(solar_os_gfx_t *gfx, int screen_w, int screen_h)
{
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 16, "SOLAR OS AMBIENT DASHBOARD");

    char top_status[48] = "";
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    solar_os_battery_status_t bat;
    if (solar_os_battery_get_status(&bat) == ESP_OK && bat.percent <= 100) {
        snprintf(top_status, sizeof(top_status), "BAT: %%%u  ", (unsigned)bat.percent);
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (wifi.connected) strlcat(top_status, "WIFI: OK", sizeof(top_status));
    else strlcat(top_status, "WIFI: OFF", sizeof(top_status));
#endif
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t sw = solar_os_gfx_text_width(gfx, top_status);
    solar_os_gfx_text(gfx, screen_w - (int)sw - 8, 16, top_status);

    /* 2. Left Side: Big Live Analog Clock Dial (Center: 95, 140, Radius: 60) */
    time_t raw_time = time(NULL);
    struct tm *t = localtime(&raw_time);
    const int hr = t != NULL ? t->tm_hour : 12;
    const int min = t != NULL ? t->tm_min : 0;
    const int sec = t != NULL ? t->tm_sec : 0;

    const int acx = 95;
    const int acy = 140;
    const int arad = 58;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_circle(gfx, acx, acy, arad);
    solar_os_gfx_circle(gfx, acx, acy, arad - 1);
    solar_os_gfx_fill_circle(gfx, acx, acy, 4);

    for (int h = 0; h < 12; h++) {
        const float a = ((float)h / 12.0f) * 2.0f * (float)M_PI;
        const int x0 = acx + (int)((float)(arad - 7) * sinf(a));
        const int y0 = acy - (int)((float)(arad - 7) * cosf(a));
        const int x1 = acx + (int)((float)arad * sinf(a));
        const int y1 = acy - (int)((float)arad * cosf(a));
        solar_os_gfx_line(gfx, x0, y0, x1, y1);
    }

    /* Hour hand */
    const float hr_angle = ((float)(hr % 12) + (float)min / 60.0f) / 12.0f * 2.0f * (float)M_PI;
    const int hx = acx + (int)(32.0f * sinf(hr_angle));
    const int hy = acy - (int)(32.0f * cosf(hr_angle));
    solar_os_gfx_line(gfx, acx, acy, hx, hy);
    solar_os_gfx_line(gfx, acx + 1, acy, hx + 1, hy);

    /* Minute hand */
    const float min_angle = ((float)min + (float)sec / 60.0f) / 60.0f * 2.0f * (float)M_PI;
    const int mx = acx + (int)(46.0f * sinf(min_angle));
    const int my = acy - (int)(46.0f * cosf(min_angle));
    solar_os_gfx_line(gfx, acx, acy, mx, my);

    /* Second hand */
    const float sec_angle = (float)sec / 60.0f * 2.0f * (float)M_PI;
    const int sx = acx + (int)(50.0f * sinf(sec_angle));
    const int sy = acy - (int)(50.0f * cosf(sec_angle));
    solar_os_gfx_line(gfx, acx, acy, sx, sy);

    /* 3. Right Side: Digital Clock & Date & Weather Summary */
    char digi_time[32];
    snprintf(digi_time, sizeof(digi_time), "%02d:%02d:%02d", hr, min, sec);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    solar_os_gfx_text(gfx, 185, 80, digi_time);

    static const char *months[] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char *wds[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const int mon = t != NULL ? t->tm_mon + 1 : 8;
    const int day = t != NULL ? t->tm_mday : 14;
    const int year = t != NULL ? t->tm_year + 1900 : 2026;
    const int wd = t != NULL ? t->tm_wday : 5;

    char date_str[64];
    snprintf(date_str, sizeof(date_str), "%s, %d %s %d", wds[wd % 7], day, months[mon % 13], year);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 185, 110, date_str);

    /* Weather Summary Card (X: 185..385, Y: 130..220) */
    solar_os_gfx_rect(gfx, 185, 130, 198, 90);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 195, 150, "WEATHER OUTLOOK");
    solar_os_gfx_line(gfx, 190, 156, 378, 156);

    draw_app_icon(gfx, 215, 185, ICON_TYPE_WEATHER, false);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_18);
    solar_os_gfx_text(gfx, 240, 184, "28 C");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 240, 204, "Istanbul - Sunny / Clear");

    /* 4. Footer */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 293, "[ Press any key to wake up and return to Desktop ]");

    solar_os_gfx_present(gfx);
}

/* ----------------- ROM Picker Modal Dialog ----------------- */

static void draw_rom_picker_modal(solar_os_gfx_t *gfx, int screen_w, int screen_h)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, 24, 30, screen_w - 48, 230);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 24, 30, screen_w - 48, 230);
    solar_os_gfx_rect(gfx, 26, 32, screen_w - 52, 226);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 38, 55, launcher.picker_title[0] ? launcher.picker_title : "FILE PICKER");
    solar_os_gfx_line(gfx, 30, 62, screen_w - 30, 62);

    if (launcher.picker_count == 0) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 40, 100, "No compatible files found!");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 40, 130, "Please place files into your SD card:");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 40, 155, "/sdcard/doc/   /sdcard/roms/   /sdcard/music/");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 40, 190, "Tip: Use 'Web File Server' in Network folder to upload files over Wi-Fi!");
        solar_os_gfx_text(gfx, 40, 235, "[ESC] Close");
    } else {
        const int list_top = 70;
        const int row_h = 24;
        const size_t max_visible = 6;
        size_t top_idx = 0;
        if (launcher.selected_picker_item >= max_visible) {
            top_idx = launcher.selected_picker_item - max_visible + 1;
        }

        for (size_t i = 0; i < max_visible && (top_idx + i) < launcher.picker_count; i++) {
            size_t idx = top_idx + i;
            const int ry = list_top + (int)i * row_h;
            const bool is_sel = (idx == launcher.selected_picker_item);

            if (is_sel) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, 34, ry, screen_w - 68, row_h - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            }

            draw_app_icon(gfx, 48, ry + 11, launcher.picker_items[idx].icon_type, is_sel);
            solar_os_gfx_set_font(gfx, is_sel ? SOLAR_OS_GFX_FONT_BOLD : SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 68, ry + 16, launcher.picker_items[idx].name);
        }

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 38, 245, "[UP/DOWN] Select | [ENTER] Open | [ESC] Cancel");
    }
}

static void launcher_draw(solar_os_context_t *ctx)
{
    if (!launcher.active) return;
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    if (launcher.view_mode == VIEW_SCREENSAVER) {
        draw_screensaver(gfx, screen_w, screen_h);
        return;
    }

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header Status Bar */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 16, "SOLAR OS");

    char top_status[64] = "";
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    solar_os_battery_status_t bat;
    if (solar_os_battery_get_status(&bat) == ESP_OK && bat.percent <= 100) {
        char bat_str[16];
        snprintf(bat_str, sizeof(bat_str), "BAT: %%%u  ", (unsigned)bat.percent);
        strlcat(top_status, bat_str, sizeof(top_status));
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    if (solar_os_ble_keyboard_is_connected()) {
        strlcat(top_status, "BLE: OK  ", sizeof(top_status));
    } else {
        strlcat(top_status, "BLE: 123456  ", sizeof(top_status));
    }
#endif
    const solar_os_input_keyboard_layout_t klayout = solar_os_input_keyboard_layout();
    if (klayout == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_TR) {
        strlcat(top_status, "KBD: TR  ", sizeof(top_status));
    } else if (klayout == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE) {
        strlcat(top_status, "KBD: DE  ", sizeof(top_status));
    } else {
        strlcat(top_status, "KBD: US  ", sizeof(top_status));
    }

#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (wifi.connected) {
        strlcat(top_status, "WIFI: OK", sizeof(top_status));
    }
#endif
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t status_w = solar_os_gfx_text_width(gfx, top_status);
    solar_os_gfx_text(gfx, screen_w - (int)status_w - 8, 16, top_status);

    /* 2. Navigation Breadcrumb Bar (Y: 26..46) with Page Indicators */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

    if (launcher.view_mode == VIEW_HOME_FOLDERS) {
        solar_os_gfx_text(gfx, 12, 42, "DESKTOP > ALL FOLDERS");
    } else {
        const launcher_folder_t *cur_f = &launcher.folders[launcher.selected_folder];
        const size_t sel = launcher.selected_item[launcher.selected_folder];
        const size_t page = sel / LAUNCHER_SUB_PAGE_SIZE;
        const size_t total_pages = (cur_f->count + LAUNCHER_SUB_PAGE_SIZE - 1) / LAUNCHER_SUB_PAGE_SIZE;

        char nav_text[96];
        if (total_pages > 1) {
            snprintf(nav_text, sizeof(nav_text), "[ ESC Back ] > %s (Page %u/%u)", cur_f->title, (unsigned)(page + 1), (unsigned)total_pages);
        } else {
            snprintf(nav_text, sizeof(nav_text), "[ ESC Back ] > %s (%u items)", cur_f->title, (unsigned)cur_f->count);
        }
        solar_os_gfx_text(gfx, 12, 42, nav_text);
    }
    solar_os_gfx_line(gfx, 10, 48, screen_w - 10, 48);

    /* 3. Grid Display with 2-Line Wrapped Text */
    if (launcher.view_mode == VIEW_HOME_FOLDERS) {
        const size_t sel = launcher.selected_folder;

        for (size_t i = 0; i < LAUNCHER_FOLDER_COUNT; i++) {
            const launcher_folder_t *f = &launcher.folders[i];
            const int col = (int)(i % LAUNCHER_GRID_COLS);
            const int row = (int)(i / LAUNCHER_GRID_COLS);

            const int card_x = LAUNCHER_GRID_START_X + col * (LAUNCHER_CARD_W + LAUNCHER_GRID_GAP_X);
            const int card_y = LAUNCHER_GRID_START_Y + row * (LAUNCHER_CARD_H + LAUNCHER_GRID_GAP_Y);
            const bool is_selected = (i == sel);

            if (is_selected) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, card_x, card_y, LAUNCHER_CARD_W, LAUNCHER_CARD_H);
                solar_os_gfx_rect(gfx, card_x - 2, card_y - 2, LAUNCHER_CARD_W + 4, LAUNCHER_CARD_H + 4);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_rect(gfx, card_x, card_y, LAUNCHER_CARD_W, LAUNCHER_CARD_H);
            }

            draw_app_icon(gfx, card_x + LAUNCHER_CARD_W / 2, card_y + 32, f->icon_type, is_selected);
            draw_wrapped_card_label(gfx, card_x, card_y, LAUNCHER_CARD_W, f->title, is_selected);
        }
    } else {
        launcher_folder_t *f = &launcher.folders[launcher.selected_folder];
        size_t sel = launcher.selected_item[launcher.selected_folder];
        if (sel >= f->count && f->count > 0) {
            sel = f->count - 1;
            launcher.selected_item[launcher.selected_folder] = sel;
        }

        if (f->count == 0) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, 60, 120, "No items in this folder yet.");
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 60, 150, "Press [ESC] to return to Desktop.");
        } else {
            const size_t page = sel / LAUNCHER_SUB_PAGE_SIZE;
            const size_t page_start = page * LAUNCHER_SUB_PAGE_SIZE;

            for (size_t i = 0; i < LAUNCHER_SUB_PAGE_SIZE && (page_start + i) < f->count; i++) {
                const size_t item_idx = page_start + i;
                const launcher_item_t *item = &f->items[item_idx];
                const int col = (int)(i % LAUNCHER_SUB_GRID_COLS);
                const int row = (int)(i / LAUNCHER_SUB_GRID_COLS);

                const int card_x = LAUNCHER_SUB_START_X + col * (LAUNCHER_SUB_CARD_W + LAUNCHER_SUB_GAP_X);
                const int card_y = LAUNCHER_SUB_START_Y + row * (LAUNCHER_SUB_CARD_H + LAUNCHER_SUB_GAP_Y);
                const bool is_selected = (item_idx == sel);

                if (is_selected) {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_fill_rect(gfx, card_x, card_y, LAUNCHER_SUB_CARD_W, LAUNCHER_SUB_CARD_H);
                    solar_os_gfx_rect(gfx, card_x - 2, card_y - 2, LAUNCHER_SUB_CARD_W + 4, LAUNCHER_SUB_CARD_H + 4);
                } else {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_rect(gfx, card_x, card_y, LAUNCHER_SUB_CARD_W, LAUNCHER_SUB_CARD_H);
                }

                draw_app_icon(gfx, card_x + LAUNCHER_SUB_CARD_W / 2, card_y + 30, item->icon_type, is_selected);
                draw_wrapped_card_label(gfx, card_x, card_y, LAUNCHER_SUB_CARD_W, item->display_label, is_selected);
            }
        }
    }

    /* 4. Description Bar (Y: 252..272) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 6, 252, screen_w - 6, 252);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
    if (launcher.notice_until_ms > now && launcher.notice_msg[0] != '\0') {
        solar_os_gfx_text(gfx, 12, 268, launcher.notice_msg);
    } else if (launcher.view_mode == VIEW_HOME_FOLDERS) {
        const launcher_folder_t *f = &launcher.folders[launcher.selected_folder];
        solar_os_gfx_text(gfx, 12, 268, f->description);
    } else {
        const launcher_folder_t *f = &launcher.folders[launcher.selected_folder];
        const size_t sel = launcher.selected_item[launcher.selected_folder];
        if (f->count > 0 && sel < f->count) {
            solar_os_gfx_text(gfx, 12, 268, f->items[sel].description);
        }
    }

    /* 5. Footer Navigation Bar (Y: 278..300) */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    if (launcher.view_mode == VIEW_HOME_FOLDERS) {
        solar_os_gfx_text(gfx, 8, 293, "[ENTER] Open Folder | [ARROWS] Navigate | [R] Refresh | [ESC] Shell");
    } else {
        solar_os_gfx_text(gfx, 8, 293, "[ENTER] Launch | [ESC] Back to Desktop | [ARROWS] Navigate | [R] Refresh");
    }

    /* 6. ROM Picker Modal */
    if (launcher.view_mode == VIEW_ROM_PICKER) {
        draw_rom_picker_modal(gfx, screen_w, screen_h);
    }

    /* 7. BLE Pairing Notice Modal Popup */
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    char b_stat[64];
    solar_os_ble_keyboard_get_status(b_stat, sizeof(b_stat));
    if (strstr(b_stat, "type") != NULL && launcher.view_mode != VIEW_ROM_PICKER) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, 40, 90, 320, 100);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, 40, 90, 320, 100);
        solar_os_gfx_rect(gfx, 42, 92, 316, 96);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 85, 120, "LOGITECH / BLE KEYBOARD PAIRING");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 60, 145, "Type the following PIN on your keyboard and press ENTER:");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 155, 172, "1 2 3 4 5 6");
    }
#endif

    solar_os_gfx_present(gfx);
}

static esp_err_t launcher_launch_item(solar_os_context_t *ctx, const launcher_item_t *item)
{
    if (item == NULL) return ESP_OK;

    if (item->kind == ITEM_KIND_GAMEBOY_LAUNCHER ||
        (item->kind == ITEM_KIND_BUILTIN && is_file_picker_app(item->name))) {
        launcher_open_file_picker_for_app(item->name);
        launcher_draw(ctx);
        return ESP_OK;
    } else if (item->kind == ITEM_KIND_GAMEBOY_ROM) {
        const solar_os_app_registry_entry_t *entry = solar_os_app_registry_find("gameboy");
        if (entry != NULL && entry->app != NULL) {
            char *argv[] = {"gameboy", (char *)item->path};
            solar_os_context_set_graphics_active(ctx, false);
            return solar_os_context_request_launch_ex(ctx, entry->app, 2, argv, SOLAR_OS_LAUNCH_CHILD_RETURN);
        }
    } else if (item->kind == ITEM_KIND_BUILTIN) {
        const solar_os_app_registry_entry_t *entry = solar_os_app_registry_find(item->name);
        if (entry != NULL && entry->app != NULL) {
            char *argv[] = {(char *)item->name};
            solar_os_context_set_graphics_active(ctx, false);
            return solar_os_context_request_launch_ex(ctx, entry->app, 1, argv, SOLAR_OS_LAUNCH_CHILD_RETURN);
        }
    } else if (item->kind == ITEM_KIND_PYTHON) {
        const solar_os_app_registry_entry_t *entry = solar_os_app_registry_find("python");
        if (entry != NULL && entry->app != NULL) {
            char *argv[] = {"python", (char *)item->path};
            solar_os_context_set_graphics_active(ctx, false);
            return solar_os_context_request_launch_ex(ctx, entry->app, 2, argv, SOLAR_OS_LAUNCH_CHILD_RETURN);
        }
    } else if (item->kind == ITEM_KIND_LUA) {
        const solar_os_app_registry_entry_t *entry = solar_os_app_registry_find("lua");
        if (entry != NULL && entry->app != NULL) {
            char *argv[] = {"lua", (char *)item->path};
            solar_os_context_set_graphics_active(ctx, false);
            return solar_os_context_request_launch_ex(ctx, entry->app, 2, argv, SOLAR_OS_LAUNCH_CHILD_RETURN);
        }
    } else if (item->kind == ITEM_KIND_ACTION_BLE_PAIR) {
#if SOLAR_OS_PACKAGE_SERVICE_BLE
        (void)solar_os_ble_keyboard_start_pairing();
        strlcpy(launcher.notice_msg, "BLE pairing started. PIN: 123456", sizeof(launcher.notice_msg));
        launcher.notice_until_ms = (uint32_t)(esp_timer_get_time() / 1000U) + 5000U;
        launcher_draw(ctx);
#endif
        return ESP_OK;
    } else if (item->kind == ITEM_KIND_ACTION_KEYBOARD_LAYOUT) {
        solar_os_input_keyboard_layout_t cur = solar_os_input_keyboard_layout();
        solar_os_input_keyboard_layout_t next = SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US;
        const char *lname = "US";

        if (cur == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US) {
            next = SOLAR_OS_INPUT_KEYBOARD_LAYOUT_TR;
            lname = "TR (Turkish Q)";
        } else if (cur == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_TR) {
            next = SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE;
            lname = "DE (German)";
        } else {
            next = SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US;
            lname = "US (English)";
        }
        (void)solar_os_input_set_keyboard_layout(next);
        snprintf(launcher.notice_msg, sizeof(launcher.notice_msg), "Keyboard layout switched to: %s", lname);
        launcher.notice_until_ms = (uint32_t)(esp_timer_get_time() / 1000U) + 4000U;
        launcher_draw(ctx);
        return ESP_OK;
    } else if (item->kind == ITEM_KIND_ACTION_SHELL) {
        solar_os_context_request_exit(ctx);
        return ESP_OK;
    } else if (item->kind == ITEM_KIND_ACTION_REBOOT) {
        esp_restart();
        return ESP_OK;
    }

    return ESP_OK;
}

static esp_err_t launcher_start(solar_os_context_t *ctx)
{
    if (ctx == NULL) return ESP_ERR_INVALID_ARG;

    launcher.ctx = ctx;
    launcher.active = true;
    launcher.view_mode = VIEW_HOME_FOLDERS;
    launcher.selected_folder = 0;
    memset(&launcher.selected_item, 0, sizeof(launcher.selected_item));
    launcher.notice_msg[0] = '\0';
    launcher.notice_until_ms = 0;
    launcher.idle_start_ms = (uint32_t)(esp_timer_get_time() / 1000U);
    launcher.screensaver_timeout_ms = 60000U; /* 1 minute default */

    solar_os_context_set_graphics_active(ctx, true);

#if SOLAR_OS_PACKAGE_SERVICE_BLE
    if (solar_os_ble_keyboard_remembered_count() == 0 && !solar_os_ble_keyboard_is_connected()) {
        (void)solar_os_ble_keyboard_start_pairing();
    }
#endif

    launcher_refresh_items();
    launcher_draw(ctx);
    return ESP_OK;
}

static void launcher_resume(solar_os_context_t *ctx)
{
    if (ctx == NULL) return;

    launcher.ctx = ctx;
    launcher.active = true;
    launcher.view_mode = VIEW_HOME_FOLDERS;
    launcher.idle_start_ms = (uint32_t)(esp_timer_get_time() / 1000U);
    solar_os_context_set_graphics_active(ctx, true);

    launcher_refresh_items();
    launcher_draw(ctx);
}

static void launcher_stop(solar_os_context_t *ctx)
{
    launcher.active = false;
    solar_os_context_set_graphics_active(ctx, false);
}

static bool launcher_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);

        /* Check Screensaver timeout */
        if (launcher.view_mode != VIEW_SCREENSAVER &&
            launcher.screensaver_timeout_ms > 0 &&
            (now - launcher.idle_start_ms >= launcher.screensaver_timeout_ms)) {
            launcher.view_mode = VIEW_SCREENSAVER;
            launcher_draw(ctx);
            return true;
        }

        if (now - launcher.last_tick_ms >= 1000U) {
            launcher.last_tick_ms = now;
            launcher_draw(ctx);
        }
        return true;
    }

    if (event->type != SOLAR_OS_EVENT_CHAR) return false;

    /* Any key activity resets the idle timer */
    launcher.idle_start_ms = (uint32_t)(esp_timer_get_time() / 1000U);

    /* If in Screensaver mode, any keypress wakes up the desktop */
    if (launcher.view_mode == VIEW_SCREENSAVER) {
        launcher.view_mode = VIEW_HOME_FOLDERS;
        launcher_draw(ctx);
        return true;
    }

    const uint32_t key = (uint32_t)(uint8_t)event->data.ch;

    /* Universal File Picker Modal Event Handling */
    if (launcher.view_mode == VIEW_ROM_PICKER) {
        if (key == SOLAR_OS_KEY_UP || key == 'w' || key == 'W' || key == 'k' || key == 'K') {
            if (launcher.selected_picker_item > 0) {
                launcher.selected_picker_item--;
                launcher_draw(ctx);
            }
            return true;
        }
        if (key == SOLAR_OS_KEY_DOWN || key == 's' || key == 'S' || key == 'j' || key == 'J') {
            if (launcher.picker_count > 0 && launcher.selected_picker_item + 1 < launcher.picker_count) {
                launcher.selected_picker_item++;
                launcher_draw(ctx);
            }
            return true;
        }
        if (key == '\r' || key == '\n') {
            if (launcher.picker_count > 0 && launcher.selected_picker_item < launcher.picker_count) {
                launcher_picker_entry_t *p = &launcher.picker_items[launcher.selected_picker_item];
                launcher.view_mode = VIEW_HOME_FOLDERS;
                const solar_os_app_registry_entry_t *entry = solar_os_app_registry_find(launcher.picker_app_name);
                if (entry != NULL && entry->app != NULL) {
                    solar_os_context_set_graphics_active(ctx, false);
                    char file_arg[128];
                    if (p->is_new_file || p->path[0] == '\0') {
                        const char *created = create_default_file_for_app(launcher.picker_app_name);
                        strlcpy(file_arg, created, sizeof(file_arg));
                    } else {
                        strlcpy(file_arg, p->path, sizeof(file_arg));
                    }
                    char *argv[] = {launcher.picker_app_name, file_arg};
                    return solar_os_context_request_launch_ex(ctx, entry->app, 2, argv, SOLAR_OS_LAUNCH_CHILD_RETURN);
                }
            }
            return true;
        }
        if (key == SOLAR_OS_KEY_ESCAPE || key == 'q' || key == 'Q' || key == '\b') {
            launcher.view_mode = VIEW_INSIDE_FOLDER;
            launcher_draw(ctx);
            return true;
        }
        return true;
    }

    if (launcher.view_mode == VIEW_HOME_FOLDERS) {
        size_t sel = launcher.selected_folder;

        switch (key) {
        case SOLAR_OS_KEY_LEFT:
        case 'a':
        case 'A':
        case 'h':
        case 'H':
            if (sel > 0) {
                launcher.selected_folder--;
            } else {
                launcher.selected_folder = LAUNCHER_FOLDER_COUNT - 1;
            }
            launcher_draw(ctx);
            return true;

        case SOLAR_OS_KEY_RIGHT:
        case 'd':
        case 'D':
        case 'l':
        case 'L':
            if (sel + 1 < LAUNCHER_FOLDER_COUNT) {
                launcher.selected_folder++;
            } else {
                launcher.selected_folder = 0;
            }
            launcher_draw(ctx);
            return true;

        case SOLAR_OS_KEY_UP:
        case 'w':
        case 'W':
        case 'k':
        case 'K':
            if (sel >= LAUNCHER_GRID_COLS) {
                launcher.selected_folder -= LAUNCHER_GRID_COLS;
            }
            launcher_draw(ctx);
            return true;

        case SOLAR_OS_KEY_DOWN:
        case 's':
        case 'S':
        case 'j':
        case 'J':
            if (sel + LAUNCHER_GRID_COLS < LAUNCHER_FOLDER_COUNT) {
                launcher.selected_folder += LAUNCHER_GRID_COLS;
            }
            launcher_draw(ctx);
            return true;

        case '\r':
        case '\n':
        case ' ':
            launcher.view_mode = VIEW_INSIDE_FOLDER;
            launcher_draw(ctx);
            return true;

        case 'r':
        case 'R':
        case SOLAR_OS_KEY_F5:
        case SOLAR_OS_KEY_ESCAPE:
#if SOLAR_OS_PACKAGE_SERVICE_BLE
            if (!solar_os_ble_keyboard_is_connected()) {
                (void)solar_os_ble_keyboard_start_pairing();
                strlcpy(launcher.notice_msg, "BLE pairing started. PIN: 123456", sizeof(launcher.notice_msg));
                launcher.notice_until_ms = (uint32_t)(esp_timer_get_time() / 1000U) + 5000U;
            } else {
                strlcpy(launcher.notice_msg, "BLE keyboard connected", sizeof(launcher.notice_msg));
                launcher.notice_until_ms = (uint32_t)(esp_timer_get_time() / 1000U) + 3000U;
            }
#endif
            launcher_refresh_items();
            launcher_draw(ctx);
            return true;

        case 3: /* Ctrl+C explicit console exit */
            solar_os_context_request_exit(ctx);
            return true;

        default:
            break;
        }
    } else {
        launcher_folder_t *f = &launcher.folders[launcher.selected_folder];
        size_t sel = launcher.selected_item[launcher.selected_folder];

        switch (key) {
        case SOLAR_OS_KEY_LEFT:
        case 'a':
        case 'A':
        case 'h':
        case 'H':
            if (sel > 0) {
                launcher.selected_item[launcher.selected_folder]--;
            } else if (f->count > 0) {
                launcher.selected_item[launcher.selected_folder] = f->count - 1;
            }
            launcher_draw(ctx);
            return true;

        case SOLAR_OS_KEY_RIGHT:
        case 'd':
        case 'D':
        case 'l':
        case 'L':
            if (f->count > 0 && sel + 1 < f->count) {
                launcher.selected_item[launcher.selected_folder]++;
            } else {
                launcher.selected_item[launcher.selected_folder] = 0;
            }
            launcher_draw(ctx);
            return true;

        case SOLAR_OS_KEY_UP:
        case 'w':
        case 'W':
        case 'k':
        case 'K':
            if (sel >= LAUNCHER_SUB_GRID_COLS) {
                launcher.selected_item[launcher.selected_folder] -= LAUNCHER_SUB_GRID_COLS;
            }
            launcher_draw(ctx);
            return true;

        case SOLAR_OS_KEY_DOWN:
        case 's':
        case 'S':
        case 'j':
        case 'J':
            if (f->count > 0 && sel + LAUNCHER_SUB_GRID_COLS < f->count) {
                launcher.selected_item[launcher.selected_folder] += LAUNCHER_SUB_GRID_COLS;
            }
            launcher_draw(ctx);
            return true;

        case '\r':
        case '\n':
            if (f->count > 0 && sel < f->count) {
                (void)launcher_launch_item(ctx, &f->items[sel]);
            }
            return true;

        case SOLAR_OS_KEY_ESCAPE:
        case '\b':
        case 127:
        case 'q':
        case 'Q':
            launcher.view_mode = VIEW_HOME_FOLDERS;
            launcher_draw(ctx);
            return true;

        case 'r':
        case 'R':
        case SOLAR_OS_KEY_F5:
#if SOLAR_OS_PACKAGE_SERVICE_BLE
            if (!solar_os_ble_keyboard_is_connected()) {
                (void)solar_os_ble_keyboard_start_pairing();
            }
#endif
            launcher_refresh_items();
            launcher_draw(ctx);
            return true;

        default:
            break;
        }
    }

    return false;
}

static uint32_t launcher_requested_tick_interval_ms(void)
{
    return 100U;
}

const solar_os_app_t solar_os_launcher_app = {
    .name = "launcher",
    .summary = "graphical desktop folder launcher",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = launcher_start,
    .resume = launcher_resume,
    .stop = launcher_stop,
    .event = launcher_event,
    .state_slot = &launcher_state_ptr,
    .state_size = sizeof(launcher_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = LAUNCHER_TASK_STACK,
    .requested_tick_interval_ms = launcher_requested_tick_interval_ms,
};
