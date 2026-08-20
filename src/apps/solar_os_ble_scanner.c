/*
 * Solar OS - Modular BLE Scanner & Device Explorer
 * Waveshare ESP32-S3 RLCD 4.2" (400x300 ST7305)
 */

#include "solar_os_ble_scanner.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "solar_os_audio.h"
#include "solar_os_appbar.h"
#include "solar_os_ble_hid.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_lua.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define TAG "ble_scanner"

#define BLE_SCANNER_MAX_DEVICES 32
#define BLE_SCANNER_MAX_BOOKMARKS 16
#define BLE_SCANNER_TASK_STACK 8192U
#define BLE_SCANNER_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define BLE_SCANNER_TICK_MS 100U
#define BLE_SCANNER_LIST_AUTORESCAN_MS 60000U /* 60s list rescan */
#define BLE_SCANNER_RADAR_RESCAN_MS 3000U     /* 3s responsive target radar rescan */


typedef enum {
    BLE_VIEW_SCANNER = 0,
    BLE_VIEW_DEVICE_PAGE = 1,
} ble_view_mode_t;

typedef enum {
    DEV_TAB_OVERVIEW = 0,
    DEV_TAB_SETTINGS = 1,
    DEV_TAB_GATT = 2,
    DEV_TAB_RADAR = 3,
    DEV_TAB_COUNT = 4,
} dev_tab_t;

typedef enum {
    BLE_FILTER_ALL = 0,
    BLE_FILTER_SAVED = 1,
    BLE_FILTER_NAMED = 2,
    BLE_FILTER_HID = 3,
    BLE_FILTER_STRONG = 4,
    BLE_FILTER_COUNT = 5,
} ble_filter_mode_t;

typedef enum {
    GATT_FOCUS_SERVICES = 0,
    GATT_FOCUS_CHARS = 1,
} gatt_focus_t;

typedef enum {
    GATT_DECODER_AUTO = 0,
    GATT_DECODER_HEX_ASCII = 1,
    GATT_DECODER_NUMERIC = 2,
    GATT_DECODER_LUA = 3,
    GATT_DECODER_COUNT = 4,
} gatt_decoder_mode_t;

typedef enum {
    ADV_INSPECT_AUTO = 0,
    ADV_INSPECT_STRUCTURES = 1,
    ADV_INSPECT_HEX = 2,
    ADV_INSPECT_LUA = 3,
    ADV_INSPECT_COUNT = 4,
} adv_inspect_mode_t;

typedef struct {
    uint8_t bda[6];
    char alias[32];
    bool alert_enabled;
    int8_t alert_threshold_rssi;
} ble_bookmark_t;

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
    int8_t rssi;
    int8_t tx_power;
    uint16_t appearance;
    bool hid_service;
    bool keyboard_like;
    bool mouse_like;
    bool gamepad_like;
    bool remembered;
    bool connected;
    char name[SOLAR_OS_BLE_KEYBOARD_NAME_MAX];
    char alias[32];
    bool is_bookmarked;
    bool is_in_range;
    uint32_t last_seen_ms;
    uint8_t adv_data[62];
    uint8_t adv_data_len;
} ble_device_item_t;

typedef struct {
    const char *text;
    bool is_header;
    bool is_code;
} ble_help_line_t;

static const ble_help_line_t ble_help_docs[] = {
    { "=== BLE SCANNER & DEVICE HUB GUIDE ===", true, false },
    { "", false, false },
    { "1. USER INTERFACE & NAVIGATION", true, false },
    { "--------------------------------------------------", false, false },
    { "[Main Scanner Screen]", true, false },
    { "* [Up/Down/Left/Right] / [WASD] : Browse detected BLE devices", false, false },
    { "* [Enter]           : Open Dedicated Device Hub", false, false },
    { "* [Space] / [R]     : Trigger instant BLE scan", false, false },
    { "* [F]               : Filter (ALL, SAVED, NAMED, HID, STRONG)", false, false },
    { "* [M]               : Manage Paired Devices (reconnect / forget)", false, false },
    { "* [H]               : Open this Help Guide", false, false },
    { "* [ESC] / [Q]       : Exit application to SolarOS", false, false },
    { "", false, false },
    { "[Device Hub - 4 Modular Tabs]", true, false },
    { "* [Tab]             : Cycle Tabs (INFO -> SETTINGS -> GATT -> RADAR)", false, false },
    { "* Tab 1 [INFO]      : Specs, live ADV broadcast & [Enter/P] Direct HID Pair", false, false },
    { "* Tab 2 [SETTINGS]  : [A] Rename Alias | [P] Proximity Alert", false, false },
    { "                      [1-4] Presets (Scale/Sensor) | [E] Edit ADV Lua", false, false },
    { "* Tab 3 [GATT]      : [C] Connect/Disconnect | [Left/Right] Switch Column", false, false },
    { "                      [Up/Down] Select | [R] Read | [M] Mode | [E] Edit Lua", false, false },
    { "* Tab 4 [RADAR]     : 3s Real-Time Proximity Tracker, Sweep & History", false, false },
    { "                      [B] Audio Proximity Beeper | [R] Scan Now", false, false },
    { "* [ESC]             : Return to Main Scanner List", false, false },
    { "", false, false },
    { "2. LUA SCRIPTING ENVIRONMENT", true, false },
    { "--------------------------------------------------", false, false },
    { "* Integrated Lua 5.3 runtime with 500ms safety timeout.", false, false },
    { "* Variables Provided to Lua Scripts:", false, false },
    { "  - bytes[1..16]    : 1-indexed raw byte payload table", false, false },
    { "  - handle          : GATT characteristic handle (integer)", false, false },
    { "  - len             : Total payload length in bytes", false, false },
    { "* Rule: Lua script MUST return a display string.", false, false },
    { "", false, false },
    { "3. PRACTICAL CODE EXAMPLES (2-LINE FORMAT)", true, false },
    { "--------------------------------------------------", false, false },
    { "Example 1: Xiaomi Mi Body Scale (ADV Payload)", true, false },
    { "  val = ((bytes[12] or 0)*256 + (bytes[11] or 0)) / 200", false, true },
    { "  return string.format('Weight: %.2f kg', val)", false, true },
    { "", false, false },
    { "Example 2: Standard Bluetooth Scale (0x181D)", true, false },
    { "  val = ((bytes[5] or 0)*256 + (bytes[4] or 0)) * 0.005", false, true },
    { "  return string.format('Scale: %.2f kg', val)", false, true },
    { "", false, false },
    { "Example 3: BTHome / Sensor (Temp & Humidity)", true, false },
    { "  t = bytes[3] or 0; h = bytes[4] or 0", false, true },
    { "  return string.format('Temp: %d C | Humidity: %d%%', t, h)", false, true },
    { "", false, false },
    { "Example 4: GATT Battery Level (0x2A19)", true, false },
    { "  lvl = bytes[1] or 0", false, true },
    { "  return string.format('Battery Level: %d%%', lvl)", false, true },
    { "", false, false },
    { "Example 5: GATT 16-bit Unsigned Little-Endian", true, false },
    { "  val = (bytes[2] or 0)*256 + (bytes[1] or 0)", false, true },
    { "  return string.format('Sensor: %u raw', val)", false, true },
    { "", false, false },
    { "Example 6: GATT ASCII Text String", true, false },
    { "  return string.char(table.unpack(bytes, 1, math.min(len, 16)))", false, true },
    { "", false, false },
    { "4. HARDWARE CONSTRAINTS & BEST PRACTICES", true, false },
    { "--------------------------------------------------", false, false },
    { "* ESP32-S3 uses a single 2.4GHz RF core.", false, false },
    { "* Background scan pauses during active GATT connection.", false, false },
    { "* Fast scan captures up to 62 bytes of ADV payload.", false, false },
    { "* Aliases & Proximity bookmarks persist in NVS flash.", false, false },
    { "", false, false },
    { "5. BLE MOUSE, GAMEPAD & MULTI-DEVICE SUPPORT", true, false },
    { "--------------------------------------------------", false, false },
    { "* Multi-Device: Simultaneously connect Keyboard, Mouse, & Gamepad!", false, false },
    { "* Supported Gamepads: Xbox Wireless, Switch Pro/Joy-Con, 8BitDo, Android HID.", false, false },
    { "* Gamepad UI Controls:", false, false },
    { "  - D-Pad / Left Stick : Arrow Keys (Up, Down, Left, Right)", false, false },
    { "  - Button A           : [Enter] / Select", false, false },
    { "  - Button B           : [ESC] / Back / Cancel", false, false },
    { "  - Button X           : [Tab] Next Field / Next Tab", false, false },
    { "  - Button Y           : [H] Help / Menu", false, false },
    { "  - Bumpers (L1/R1)    : [PageUp] / [PageDown] Fast Scroll", false, false },
    { "  - Triggers (L2/R2)   : Volume Down / Volume Up", false, false },
    { "* BLE Mouse Controls:", false, false },
    { "  - Smooth on-screen pointer cursor (auto-hides after 5s)", false, false },
    { "  - Scroll Wheel       : [Up] / [Down] menu & list scrolling", false, false },
    { "  - Left Click         : [Enter] / Select", false, false },
    { "  - Right Click        : [ESC] / Back", false, false },
    { "", false, false },
    { "=== End of Guide - Press [ESC] or [H] to Exit ===", true, false },
};

typedef struct {
    /* --- Scan worker (guarded by ble_scanner_lock) --- */
    TaskHandle_t task;
    volatile bool task_done;
    solar_os_ble_keyboard_scan_result_t staging_results[BLE_SCANNER_MAX_DEVICES];
    size_t staging_count;
    esp_err_t staging_err;
    bool staging_ready;

    /* --- Main thread state --- */
    bool scanning;
    ble_device_item_t devices[BLE_SCANNER_MAX_DEVICES];
    size_t device_count;
    size_t selected_device;
    ble_filter_mode_t filter;
    ble_view_mode_t view;
    dev_tab_t active_tab;
    uint32_t next_auto_rescan_ms;

    /* Bookmarks / Saved Devices */
    ble_bookmark_t bookmarks[BLE_SCANNER_MAX_BOOKMARKS];
    size_t bookmark_count;

    /* Alias editing modal */
    bool editing_alias;
    char alias_input[32];
    size_t alias_cursor;

    /* --- ADV Parser & Lua State --- */
    adv_inspect_mode_t adv_mode;
    char custom_lua_expr[160];
    bool editing_lua;
    char lua_input[160];
    size_t lua_cursor;

    /* --- Selected Device & GATT Explorer State --- */
    bool gatt_connecting;
    bool gatt_connected;
    uint8_t target_bda[6];
    uint8_t target_addr_type;
    char target_name[SOLAR_OS_BLE_KEYBOARD_NAME_MAX];
    char target_alias[32];
    int8_t target_rssi;
    solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
    size_t service_count;
    size_t selected_service;

    solar_os_ble_gatt_characteristic_t characteristics[SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS];
    size_t char_count;
    size_t selected_char;
    gatt_focus_t gatt_focus;

    /* Live characteristic read value & decoders */
    uint8_t read_value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t read_value_len;
    uint16_t last_read_handle;
    bool has_read_value;
    gatt_decoder_mode_t decoder_mode;
    char custom_gatt_lua_expr[160];
    bool editing_gatt_lua;
    char gatt_lua_input[160];
    size_t gatt_lua_cursor;

    /* --- Radar State --- */
    int8_t rssi_history[32];
    size_t rssi_history_count;
    uint32_t next_beep_ms;
    bool beep_enabled;
    float radar_sweep_angle;

    /* Help Modal & Scroll */
    bool showing_help;
    size_t help_scroll_line;

    /* Paired Devices Modal */
    bool showing_paired;
    size_t paired_selected;

    /* General status */
    uint32_t elapsed_ms;
    bool render_pending;
    char status_message[80];
    uint32_t status_until_ms;
} ble_scanner_state_t;

static void *ble_scanner_state_ptr;
#define ble_state (*(ble_scanner_state_t *)ble_scanner_state_ptr)

SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("ble_scanner worker spinlock")
static portMUX_TYPE ble_scanner_lock = portMUX_INITIALIZER_UNLOCKED;

static void ble_scanner_render(solar_os_context_t *ctx);
static void ble_scanner_set_status(const char *message);
static void ble_scanner_load_bookmarks(void);
static void ble_scanner_save_bookmarks(void);
static void ble_start_device_gatt_connect(void);

/* ---------------------------------------------------------------------
 * Bluetooth SIG Standard UUID & Company ID Resolvers
 * ------------------------------------------------------------------- */

static const char *ble_resolve_company_name(uint16_t company_id)
{
    switch (company_id) {
    case 0x004C: return "Apple Inc.";
    case 0x0006: return "Microsoft";
    case 0x02E5: return "Espressif Systems";
    case 0x00E0: return "Google LLC";
    case 0x0075: return "Samsung Electronics";
    case 0x038F: return "Xiaomi Inc.";
    case 0x0157: return "Huami / Amazfit";
    case 0x0087: return "Garmin";
    case 0x0059: return "Nordic Semi";
    case 0x000D: return "Texas Instruments";
    case 0x013A: return "Fitbit";
    case 0x0046: return "Sony";
    case 0x0002: return "Intel";
    case 0x000A: return "Qualcomm";
    case 0x0100: return "LG Electronics";
    case 0x0060: return "Logitech";
    case 0x03C2: return "Bose";
    case 0x07D7: return "Huawei";
    default: return "Vendor Custom";
    }
}

static const char *ble_resolve_uuid_name(const char *uuid_str)
{
    if (uuid_str == NULL || uuid_str[0] == '\0') {
        return "Unknown Service";
    }

    if (strcasecmp(uuid_str, "0x1800") == 0) return "Generic Access";
    if (strcasecmp(uuid_str, "0x1801") == 0) return "Generic Attribute";
    if (strcasecmp(uuid_str, "0x1802") == 0) return "Immediate Alert";
    if (strcasecmp(uuid_str, "0x1803") == 0) return "Link Loss";
    if (strcasecmp(uuid_str, "0x1804") == 0) return "Tx Power";
    if (strcasecmp(uuid_str, "0x1805") == 0) return "Current Time";
    if (strcasecmp(uuid_str, "0x1808") == 0) return "Glucose";
    if (strcasecmp(uuid_str, "0x1809") == 0) return "Health Thermometer";
    if (strcasecmp(uuid_str, "0x180a") == 0) return "Device Info";
    if (strcasecmp(uuid_str, "0x180d") == 0) return "Heart Rate";
    if (strcasecmp(uuid_str, "0x180f") == 0) return "Battery Service";
    if (strcasecmp(uuid_str, "0x1810") == 0) return "Blood Pressure";
    if (strcasecmp(uuid_str, "0x1812") == 0) return "HID Service";
    if (strcasecmp(uuid_str, "0x181a") == 0) return "Environmental";
    if (strcasecmp(uuid_str, "0x181b") == 0) return "Body Composition";
    if (strcasecmp(uuid_str, "0x181d") == 0) return "Weight Scale (Baskul)";
    if (strcasecmp(uuid_str, "0x1820") == 0) return "IP Support";
    if (strcasecmp(uuid_str, "0x1822") == 0) return "Pulse Oximeter";

    /* Characteristics */
    if (strcasecmp(uuid_str, "0x2a00") == 0) return "Device Name";
    if (strcasecmp(uuid_str, "0x2a01") == 0) return "Appearance";
    if (strcasecmp(uuid_str, "0x2a19") == 0) return "Battery Level (%)";
    if (strcasecmp(uuid_str, "0x2a24") == 0) return "Model Number";
    if (strcasecmp(uuid_str, "0x2a25") == 0) return "Serial Number";
    if (strcasecmp(uuid_str, "0x2a26") == 0) return "Firmware Rev";
    if (strcasecmp(uuid_str, "0x2a28") == 0) return "Software Rev";
    if (strcasecmp(uuid_str, "0x2a29") == 0) return "Manufacturer";
    if (strcasecmp(uuid_str, "0x2a4a") == 0) return "HID Info";
    if (strcasecmp(uuid_str, "0x2a4b") == 0) return "Report Map";
    if (strcasecmp(uuid_str, "0x2a4d") == 0) return "Report";
    if (strcasecmp(uuid_str, "0x2a6e") == 0) return "Temperature";
    if (strcasecmp(uuid_str, "0x2a6f") == 0) return "Humidity";
    if (strcasecmp(uuid_str, "0x2a98") == 0) return "Weight Meas";
    if (strcasecmp(uuid_str, "0x2a9c") == 0) return "Body Comp Meas";
    if (strcasecmp(uuid_str, "0x2a37") == 0) return "Heart Rate";

    return "Custom UUID";
}

/* ---------------------------------------------------------------------
 * Bookmarks & Custom Aliases Persistence (NVS)
 * ------------------------------------------------------------------- */

static void ble_scanner_load_bookmarks(void)
{
    nvs_handle_t handle;
    if (nvs_open("ble_book", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t size = sizeof(ble_state.bookmarks);
    if (nvs_get_blob(handle, "items", ble_state.bookmarks, &size) == ESP_OK) {
        ble_state.bookmark_count = size / sizeof(ble_bookmark_t);
        if (ble_state.bookmark_count > BLE_SCANNER_MAX_BOOKMARKS) {
            ble_state.bookmark_count = BLE_SCANNER_MAX_BOOKMARKS;
        }
    }
    nvs_close(handle);
}

static void ble_scanner_save_bookmarks(void)
{
    nvs_handle_t handle;
    if (nvs_open("ble_book", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (ble_state.bookmark_count > 0) {
        nvs_set_blob(handle, "items", ble_state.bookmarks, ble_state.bookmark_count * sizeof(ble_bookmark_t));
    } else {
        nvs_erase_key(handle, "items");
    }
    nvs_commit(handle);
    nvs_close(handle);
}

static ble_bookmark_t *ble_find_bookmark(const uint8_t bda[6])
{
    if (bda == NULL) return NULL;
    for (size_t i = 0; i < ble_state.bookmark_count; i++) {
        if (memcmp(ble_state.bookmarks[i].bda, bda, 6) == 0) {
            return &ble_state.bookmarks[i];
        }
    }
    return NULL;
}

static void ble_sync_device_bookmarks(void)
{
    for (size_t i = 0; i < ble_state.device_count; i++) {
        ble_device_item_t *dev = &ble_state.devices[i];
        const ble_bookmark_t *bm = ble_find_bookmark(dev->bda);
        if (bm != NULL) {
            dev->is_bookmarked = true;
            strlcpy(dev->alias, bm->alias, sizeof(dev->alias));
        } else {
            dev->is_bookmarked = false;
            dev->alias[0] = '\0';
        }
    }
}

static solar_os_ble_keyboard_scan_result_t s_worker_scan_buf[BLE_SCANNER_MAX_DEVICES];

static void ble_scanner_scan_worker(void *arg)
{
    (void)arg;
    for (;;) {
        size_t found = 0U;
        const esp_err_t err = solar_os_ble_scan_start(3, s_worker_scan_buf, BLE_SCANNER_MAX_DEVICES, &found);

        portENTER_CRITICAL(&ble_scanner_lock);
        if (err == ESP_OK) {
            memcpy(ble_state.staging_results, s_worker_scan_buf, found * sizeof(ble_state.staging_results[0]));
            ble_state.staging_count = found;
        } else {
            ble_state.staging_count = 0U;
        }
        ble_state.staging_err = err;
        ble_state.staging_ready = true;
        ble_state.task_done = true;
        portEXIT_CRITICAL(&ble_scanner_lock);

        vTaskSuspend(NULL);
    }
}

static void ble_scanner_start_scan(void)
{
    if (ble_state.scanning) return;

    portENTER_CRITICAL(&ble_scanner_lock);
    ble_state.staging_ready = false;
    ble_state.task_done = false;
    portEXIT_CRITICAL(&ble_scanner_lock);

    if (ble_state.task != NULL) {
        vTaskResume(ble_state.task);
        ble_state.scanning = true;
        ble_state.render_pending = true;
        return;
    }

    const BaseType_t created = solar_os_task_create_pinned_internal(
        ble_scanner_scan_worker,
        "ble_scan_wrk",
        BLE_SCANNER_TASK_STACK,
        NULL,
        BLE_SCANNER_TASK_PRIORITY,
        &ble_state.task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_SYSTEM
    );

    if (created == pdPASS && ble_state.task != NULL) {
        ble_state.scanning = true;
        ble_state.render_pending = true;
    } else {
        ble_scanner_set_status("Scan task creation failed");
    }
}

static void ble_scanner_stop_worker(void)
{
    if (ble_state.task != NULL) {
        vTaskDelete(ble_state.task);
        ble_state.task = NULL;
    }
    ble_state.scanning = false;
}

/* ---------------------------------------------------------------------
 * Filter & Selection Logic
 * ------------------------------------------------------------------- */

static bool ble_device_matches_filter(const ble_device_item_t *dev, ble_filter_mode_t filter)
{
    if (dev == NULL) return false;
    switch (filter) {
    case BLE_FILTER_ALL:
        return true;
    case BLE_FILTER_SAVED:
        return dev->is_bookmarked;
    case BLE_FILTER_NAMED:
        return dev->name[0] != '\0' && strcmp(dev->name, "(none)") != 0;
    case BLE_FILTER_HID:
        return dev->hid_service || dev->keyboard_like || dev->mouse_like || dev->gamepad_like;
    case BLE_FILTER_STRONG:
        return dev->rssi >= -70;
    default:
        return true;
    }
}

static size_t ble_filtered_device_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < ble_state.device_count; i++) {
        if (ble_device_matches_filter(&ble_state.devices[i], ble_state.filter)) {
            count++;
        }
    }
    return count;
}

static ble_device_item_t *ble_get_filtered_device(size_t index)
{
    size_t current = 0;
    for (size_t i = 0; i < ble_state.device_count; i++) {
        if (ble_device_matches_filter(&ble_state.devices[i], ble_state.filter)) {
            if (current == index) {
                return &ble_state.devices[i];
            }
            current++;
        }
    }
    return NULL;
}

static ble_device_item_t *ble_find_device(const uint8_t bda[6])
{
    if (bda == NULL) return NULL;
    for (size_t i = 0; i < ble_state.device_count; i++) {
        if (memcmp(ble_state.devices[i].bda, bda, 6) == 0) {
            return &ble_state.devices[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------
 * GATT Explorer Routines
 * ------------------------------------------------------------------- */

static void ble_load_service_characteristics(size_t srv_idx)
{
    if (!ble_state.gatt_connected || srv_idx >= ble_state.service_count) return;

    size_t count = 0;
    const esp_err_t err = solar_os_ble_gatt_characteristics(srv_idx,
                                                           ble_state.characteristics,
                                                           SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS,
                                                           &count);
    if (err == ESP_OK) {
        ble_state.char_count = count;
        ble_state.selected_char = 0;
        ble_state.has_read_value = false;
    } else {
        ble_state.char_count = 0;
    }
    ble_state.render_pending = true;
}

static void ble_start_device_gatt_connect(void)
{
    /* Stop background scan worker so radio is 100% dedicated to GATT */
    ble_scanner_stop_worker();

    ble_state.gatt_connecting = true;
    ble_state.gatt_connected = false;
    ble_state.service_count = 0;
    ble_state.selected_service = 0;
    ble_state.char_count = 0;
    ble_state.selected_char = 0;
    ble_state.gatt_focus = GATT_FOCUS_SERVICES;
    ble_state.has_read_value = false;

    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to %s...", ble_state.target_alias[0] ? ble_state.target_alias : ble_state.target_name);
    ble_scanner_set_status(msg);

    (void)solar_os_ble_gatt_disconnect();

    const esp_err_t err = solar_os_ble_gatt_connect(ble_state.target_bda, ble_state.target_addr_type, 6000);
    if (err == ESP_OK) {
        ble_state.gatt_connected = true;
        ble_state.gatt_connecting = false;

        size_t srv_count = 0;
        if (solar_os_ble_gatt_services(ble_state.services, SOLAR_OS_BLE_GATT_MAX_SERVICES, &srv_count) == ESP_OK) {
            ble_state.service_count = srv_count;
            if (srv_count > 0) {
                ble_load_service_characteristics(0);
            }
            snprintf(msg, sizeof(msg), "Connected: %u Services found", (unsigned)srv_count);
            ble_scanner_set_status(msg);
        } else {
            ble_scanner_set_status("Connected, no services found");
        }
    } else {
        ble_state.gatt_connecting = false;
        ble_state.gatt_connected = false;
        snprintf(msg, sizeof(msg), "Connect failed: %s (Beacon / Non-GATT)", esp_err_to_name(err));
        ble_scanner_set_status(msg);
    }
    ble_state.render_pending = true;
}

static void ble_read_selected_characteristic(void)
{
    if (!ble_state.gatt_connected || ble_state.char_count == 0) return;
    if (ble_state.selected_char >= ble_state.char_count) return;

    const uint16_t handle = ble_state.characteristics[ble_state.selected_char].handle;
    size_t read_len = 0;
    ble_scanner_set_status("Reading characteristic...");

    const esp_err_t err = solar_os_ble_gatt_read(handle,
                                                  ble_state.read_value,
                                                  SOLAR_OS_BLE_GATT_VALUE_MAX,
                                                  &read_len,
                                                  3000);
    if (err == ESP_OK) {
        ble_state.read_value_len = read_len;
        ble_state.last_read_handle = handle;
        ble_state.has_read_value = true;
        char msg[64];
        snprintf(msg, sizeof(msg), "Read 0x%04X: %u bytes", (unsigned)handle, (unsigned)read_len);
        ble_scanner_set_status(msg);
    } else {
        ble_scanner_set_status("GATT Read failed");
    }
    ble_state.render_pending = true;
}

/* ---------------------------------------------------------------------
 * Status Message Helper
 * ------------------------------------------------------------------- */

static void ble_scanner_set_status(const char *message)
{
    strncpy(ble_state.status_message, message, sizeof(ble_state.status_message) - 1U);
    ble_state.status_message[sizeof(ble_state.status_message) - 1U] = '\0';
    ble_state.status_until_ms = ble_state.elapsed_ms + 3500U;
    ble_state.render_pending = true;
}

/* ---------------------------------------------------------------------
 * UI Drawing Helpers
 * ------------------------------------------------------------------- */

static const char * const BLE_DEV_TAB_NAMES[DEV_TAB_COUNT] = { "Info", "Settings", "GATT", "Radar" };

static void ble_build_header(solar_os_appbar_header_t *out, char *status_buf, size_t status_buf_len)
{
    memset(out, 0, sizeof(*out));
    out->show_back = true;

    if (ble_state.view == BLE_VIEW_SCANNER) {
        out->title = "BLE Scanner";
        snprintf(status_buf, status_buf_len, "Devices: %u | %s",
                 (unsigned)ble_filtered_device_count(),
                 ble_state.scanning ? "SCANNING..." : "READY");
        out->status_line = status_buf;
    } else {
        out->title = ble_state.target_alias[0] ? ble_state.target_alias :
                     (ble_state.target_name[0] ? ble_state.target_name : "Device");
        out->tabs.names = BLE_DEV_TAB_NAMES;
        out->tabs.count = DEV_TAB_COUNT;
        out->tabs.active_index = (size_t)ble_state.active_tab;
    }
}

static void ble_draw_header(solar_os_gfx_t *gfx)
{
    char status_buf[64];
    solar_os_appbar_header_t header;
    ble_build_header(&header, status_buf, sizeof(status_buf));
    solar_os_appbar_draw_header(gfx, &header);
}

/* Builds the current footer's shortcut chips into a caller-owned buffer,
 * returning the count. Same set used by both drawing and click hit-testing
 * so they can never disagree about what's on screen. */
static size_t ble_build_footer_shortcuts(solar_os_appbar_shortcut_t *items, size_t max_items)
{
    size_t n = 0;
    if (ble_state.showing_paired) {
        if (n < max_items) { items[n].key = 'D'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Forget"); n++; }
        if (n < max_items) { items[n].key = (char)SOLAR_OS_KEY_ESCAPE; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Close"); n++; }
        return n;
    }

    if (ble_state.view == BLE_VIEW_SCANNER) {
        const char *filter_name = ble_state.filter == BLE_FILTER_ALL ? "All" :
                                  ble_state.filter == BLE_FILTER_SAVED ? "Saved" :
                                  ble_state.filter == BLE_FILTER_NAMED ? "Named" :
                                  ble_state.filter == BLE_FILTER_HID ? "HID" : "Strong";
        if (n < max_items) { items[n].key = 'R'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Scan"); n++; }
        if (n < max_items) { items[n].key = 'F'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Filter: %s", filter_name); n++; }
        if (n < max_items) { items[n].key = 'M'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Paired"); n++; }
        if (n < max_items) { items[n].key = 'H'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Help"); n++; }
        return n;
    }

    switch (ble_state.active_tab) {
    case DEV_TAB_OVERVIEW: {
        const ble_device_item_t *d = ble_find_device(ble_state.target_bda);
        const bool connected = d != NULL && d->connected;
        if (n < max_items) { items[n].key = connected ? 'D' : 'P'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "%s", connected ? "Disconnect" : "Connect"); n++; }
        break;
    }
    case DEV_TAB_SETTINGS:
        if (n < max_items) { items[n].key = 'A'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Rename"); n++; }
        if (n < max_items) { items[n].key = 'E'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Edit Lua"); n++; }
        if (n < max_items) { items[n].key = 'P'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Alert"); n++; }
        break;
    case DEV_TAB_GATT:
        if (n < max_items) { items[n].key = 'C'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "%s", ble_state.gatt_connected ? "Disconnect" : "Connect"); n++; }
        if (n < max_items) { items[n].key = 'R'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Read"); n++; }
        if (n < max_items) { items[n].key = 'M'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Mode"); n++; }
        if (n < max_items) { items[n].key = 'E'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Lua"); n++; }
        break;
    case DEV_TAB_RADAR:
    default:
        if (n < max_items) { items[n].key = 'B'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "%s", ble_state.beep_enabled ? "Beep Off" : "Beep On"); n++; }
        if (n < max_items) { items[n].key = 'R'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Scan Now"); n++; }
        break;
    }
    return n;
}

static void ble_draw_footer(solar_os_gfx_t *gfx, int width, int height)
{
    if (ble_state.status_until_ms > ble_state.elapsed_ms && ble_state.status_message[0] != '\0' &&
        !ble_state.showing_paired) {
        const int footer_h = solar_os_appbar_footer_height(gfx);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_rect(gfx, 0, height - footer_h, width, footer_h);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 8, height - footer_h / 4, ble_state.status_message);
        return;
    }

    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = ble_build_footer_shortcuts(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);
}

static void ble_draw_signal_bars(solar_os_gfx_t *gfx, int x, int y, int8_t rssi)
{
    int bars = 1;
    if (rssi >= -60) bars = 4;
    else if (rssi >= -75) bars = 3;
    else if (rssi >= -85) bars = 2;

    for (int b = 0; b < 4; b++) {
        const int bx = x + b * 4;
        const int bh = (b + 1) * 3;
        const int by = y - bh;
        if (b < bars) {
            solar_os_gfx_fill_rect(gfx, bx, by, 3, bh);
        } else {
            solar_os_gfx_rect(gfx, bx, by, 3, bh);
        }
    }
}

/* ---------------------------------------------------------------------
 * View 0: BLE Device Scanner List (2-Line Row Layout)
 * ------------------------------------------------------------------- */

/* Shared geometry for the scanner list body: computed once, used by both
 * drawing and click hit-testing so they can never disagree about layout. */
typedef struct {
    int top;
    int row_h;
    int visible_rows;
    size_t total_visible;
    size_t scroll_offset;
} ble_scanner_list_layout_t;

static void ble_scanner_layout_list(solar_os_gfx_t *gfx, int height, ble_scanner_list_layout_t *out)
{
    out->top = solar_os_appbar_header_height(gfx) + solar_os_appbar_status_line_height(gfx) + 4;
    out->row_h = 32;
    const int bottom = height - solar_os_appbar_footer_height(gfx) - 4;
    out->visible_rows = (bottom - out->top) / out->row_h;
    out->total_visible = ble_filtered_device_count();

    if (out->total_visible > 0 && ble_state.selected_device >= out->total_visible) {
        ble_state.selected_device = out->total_visible - 1U;
    }

    out->scroll_offset = 0;
    if (out->total_visible > 0 && ble_state.selected_device >= (size_t)out->visible_rows) {
        out->scroll_offset = ble_state.selected_device - (size_t)out->visible_rows + 1U;
    }
}

/* Returns the filtered-device index under (x, y) in the scanner list body,
 * or false if the tap missed every row (empty space, header, footer). */
static bool ble_scanner_hit_test_list(solar_os_gfx_t *gfx, int width, int height,
                                      int16_t x, int16_t y, size_t *out_index)
{
    ble_scanner_list_layout_t layout;
    ble_scanner_layout_list(gfx, height, &layout);
    if (layout.total_visible == 0) return false;
    if (x < 4 || x >= width - 4) return false;
    if (y < layout.top) return false;

    const int row_in = (y - layout.top) / layout.row_h;
    if (row_in < 0 || row_in >= layout.visible_rows) return false;

    const size_t dev_idx = layout.scroll_offset + (size_t)row_in;
    if (dev_idx >= layout.total_visible) return false;

    *out_index = dev_idx;
    return true;
}

static void ble_draw_scanner_list(solar_os_gfx_t *gfx, int width, int height)
{
    ble_scanner_list_layout_t layout;
    ble_scanner_layout_list(gfx, height, &layout);
    const int top = layout.top;
    const int row_h = layout.row_h;
    const int visible_rows = layout.visible_rows;
    const size_t total_visible = layout.total_visible;
    const size_t scroll_offset = layout.scroll_offset;

    if (total_visible == 0) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        if (ble_state.scanning) {
            solar_os_gfx_text(gfx, 20, top + 40, "Scanning for Bluetooth Low Energy devices...");
        } else {
            solar_os_gfx_text(gfx, 20, top + 40, "No BLE devices found. Press [Space] or [R] to scan.");
        }
        return;
    }

    for (int r = 0; r < visible_rows; r++) {
        const size_t dev_idx = scroll_offset + (size_t)r;
        if (dev_idx >= total_visible) break;

        const ble_device_item_t *dev = ble_get_filtered_device(dev_idx);
        if (dev == NULL) continue;

        const int y = top + r * row_h;
        const bool is_selected = (dev_idx == ble_state.selected_device);

        if (is_selected) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 4, y, width - 8, row_h - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, 4, y, width - 8, row_h - 2);
        }

        /* Signal Meter */
        ble_draw_signal_bars(gfx, 8, y + 20, dev->rssi);

        /* Line 1: Alias / Device Name */
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        char label[64];
        if (dev->alias[0] != '\0') {
            snprintf(label, sizeof(label), "* [%s]", dev->alias);
        } else if (dev->name[0] != '\0' && strcmp(dev->name, "(none)") != 0) {
            snprintf(label, sizeof(label), "%s", dev->name);
        } else {
            snprintf(label, sizeof(label), "(Unknown BLE Device)");
        }
        solar_os_gfx_text(gfx, 28, y + 13, label);

        /* Line 1 (Right): Tags */
        char tags[48] = "";
        if (dev->adv_data_len > 0) {
            char adv_tag[16];
            snprintf(adv_tag, sizeof(adv_tag), "[ADV %uB] ", (unsigned)dev->adv_data_len);
            strlcat(tags, adv_tag, sizeof(tags));
        }
        if (dev->gamepad_like) strlcat(tags, "[PAD] ", sizeof(tags));
        else if (dev->mouse_like) strlcat(tags, "[MOUSE] ", sizeof(tags));
        else if (dev->keyboard_like) strlcat(tags, "[KBD] ", sizeof(tags));
        else if (dev->hid_service) strlcat(tags, "[HID] ", sizeof(tags));
        if (tags[0] != '\0') {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, width - 120, y + 13, tags);
        }

        /* Line 2: MAC Address & Signal & Original Name if aliased */
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        char line2[80];
        if (dev->alias[0] != '\0' && dev->name[0] != '\0' && strcmp(dev->name, "(none)") != 0 && strcmp(dev->alias, dev->name) != 0) {
            snprintf(line2, sizeof(line2), "MAC: %02X:%02X:%02X:%02X:%02X:%02X  %ddBm  (%s)",
                     dev->bda[0], dev->bda[1], dev->bda[2],
                     dev->bda[3], dev->bda[4], dev->bda[5],
                     (int)dev->rssi, dev->name);
        } else {
            snprintf(line2, sizeof(line2), "MAC: %02X:%02X:%02X:%02X:%02X:%02X  RSSI: %ddBm",
                     dev->bda[0], dev->bda[1], dev->bda[2],
                     dev->bda[3], dev->bda[4], dev->bda[5],
                     (int)dev->rssi);
        }
        solar_os_gfx_text(gfx, 28, y + 26, line2);
    }
}

/* ---------------------------------------------------------------------
 * ADV Data & Sensor Decoders
 * ------------------------------------------------------------------- */

static void ble_render_adv_decoded_box(solar_os_gfx_t *gfx, const ble_device_item_t *dev, int x, int y, int w, int h)
{
    solar_os_gfx_rect(gfx, x, y, w, h);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Broadcast Data (Payload: %u Bytes):", (unsigned)dev->adv_data_len);
    solar_os_gfx_text(gfx, x + 6, y + 14, hdr);

    if (dev->adv_data_len == 0) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, x + 6, y + 32, "(No advertising packet captured yet. Press [R] to scan)");
        return;
    }

    const uint8_t *adv = dev->adv_data;
    const size_t len = dev->adv_data_len;
    int cur_y = y + 32;

    /* 1. Custom Lua evaluation if configured */
    if (ble_state.custom_lua_expr[0] != '\0') {
        char lua_code[256];
        snprintf(lua_code, sizeof(lua_code),
                 "bytes = {%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u}; "
                 "%s",
                 len > 0 ? adv[0] : 0, len > 1 ? adv[1] : 0, len > 2 ? adv[2] : 0, len > 3 ? adv[3] : 0,
                 len > 4 ? adv[4] : 0, len > 5 ? adv[5] : 0, len > 6 ? adv[6] : 0, len > 7 ? adv[7] : 0,
                 len > 8 ? adv[8] : 0, len > 9 ? adv[9] : 0, len > 10 ? adv[10] : 0, len > 11 ? adv[11] : 0,
                 len > 12 ? adv[12] : 0, len > 13 ? adv[13] : 0, len > 14 ? adv[14] : 0, len > 15 ? adv[15] : 0,
                 ble_state.custom_lua_expr);
        char out_buf[96] = "";
        solar_os_script_run_request_t req = {
            .input_type = SOLAR_OS_SCRIPT_INPUT_SOURCE,
            .input = lua_code,
            .input_len = strlen(lua_code),
            .source_name = "ble_adv_eval",
            .timeout_ms = 500,
            .output = out_buf,
            .output_size = sizeof(out_buf),
        };
        solar_os_script_run_result_t res = {0};
        if (solar_os_lua_run(&req, &res) == ESP_OK && res.success && out_buf[0] != '\0') {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
            solar_os_gfx_text(gfx, x + 6, cur_y + 4, out_buf);
            return;
        }
    }

    /* 2. Check for Scales (Xiaomi, Standard 0x181D) */
    bool decoded = false;
    for (size_t i = 0; i + 1 < len;) {
        uint8_t ad_len = adv[i];
        if (ad_len == 0 || i + 1 + ad_len > len) break;
        uint8_t ad_type = adv[i + 1];
        const uint8_t *ad_payload = &adv[i + 2];
        uint8_t payload_len = ad_len - 1;

        if (ad_type == 0x16 && payload_len >= 4) {
            uint16_t srv_uuid = (uint16_t)ad_payload[0] | ((uint16_t)ad_payload[1] << 8);
            if (srv_uuid == 0x181D || srv_uuid == 0x181B) {
                uint8_t flags = ad_payload[2];
                const char *unit = (flags & 0x01) ? "lbs" : "kg";
                uint16_t raw_w = (uint16_t)ad_payload[3] | ((uint16_t)ad_payload[4] << 8);
                float weight_kg = (flags & 0x01) ? ((float)raw_w * 0.01f) : ((float)raw_w * 0.005f);
                char scale_buf[80];
                snprintf(scale_buf, sizeof(scale_buf), "WEIGHT SCALE (BASKUL): %.2f %s", weight_kg, unit);
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
                solar_os_gfx_text(gfx, x + 6, cur_y + 4, scale_buf);
                decoded = true;
                break;
            }
        }

        if (ad_type == 0xFF && payload_len >= 12) {
            uint16_t comp_id = (uint16_t)ad_payload[0] | ((uint16_t)ad_payload[1] << 8);
            if (comp_id == 0x038F || comp_id == 0x0157) {
                uint8_t flags = ad_payload[2];
                const char *unit = (flags & 0x01) ? "lbs" : ((flags & 0x02) ? "jin" : "kg");
                bool stabilized = (flags & 0x20) != 0;
                uint16_t raw_w = (uint16_t)ad_payload[11] | ((uint16_t)ad_payload[12] << 8);
                float w = (flags & 0x01) ? ((float)raw_w / 100.0f) : ((float)raw_w / 200.0f);
                char scale_buf[80];
                snprintf(scale_buf, sizeof(scale_buf), "XIAOMI SCALE: %.2f %s [%s]",
                         w, unit, stabilized ? "STABILIZED" : "MEASURING...");
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
                solar_os_gfx_text(gfx, x + 6, cur_y + 4, scale_buf);
                decoded = true;
                break;
            }
        }
        i += (1 + ad_len);
    }

    if (decoded) return;

    /* 3. General TLV Elements or Manufacturer Info */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char hex_str[80] = "Hex: ";
    for (size_t k = 0; k < len && k < 14; k++) {
        char h[8];
        snprintf(h, sizeof(h), "%02X ", adv[k]);
        strlcat(hex_str, h, sizeof(hex_str));
    }
    solar_os_gfx_text(gfx, x + 6, cur_y, hex_str);

    /* Company Name if present */
    for (size_t i = 0; i + 1 < len;) {
        uint8_t ad_len = adv[i];
        if (ad_len == 0 || i + 1 + ad_len > len) break;
        uint8_t ad_type = adv[i + 1];
        const uint8_t *ad_payload = &adv[i + 2];
        uint8_t payload_len = ad_len - 1;
        if (ad_type == 0xFF && payload_len >= 2) {
            uint16_t comp_id = (uint16_t)ad_payload[0] | ((uint16_t)ad_payload[1] << 8);
            char comp_str[64];
            snprintf(comp_str, sizeof(comp_str), "Manufacturer: 0x%04X (%s)", comp_id, ble_resolve_company_name(comp_id));
            solar_os_gfx_text(gfx, x + 6, cur_y + 16, comp_str);
            break;
        }
        i += (1 + ad_len);
    }
}

/* ---------------------------------------------------------------------
 * View 1: Device Page - Tab 0: Overview & Live Data
 * ------------------------------------------------------------------- */

static void ble_draw_tab_overview(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = solar_os_appbar_header_height(gfx) + 4;
    const ble_device_item_t *dev = ble_find_device(ble_state.target_bda);
    if (dev == NULL) return;

    /* 1. Hardware Summary Box */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 6, top, width - 12, 70);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char title[80];
    if (dev->alias[0] != '\0' && dev->name[0] != '\0' && strcmp(dev->alias, dev->name) != 0) {
        snprintf(title, sizeof(title), "* [%s] (%s)", dev->alias, dev->name);
    } else {
        snprintf(title, sizeof(title), "%s", dev->alias[0] ? dev->alias : (dev->name[0] ? dev->name : "(Unnamed Device)"));
    }
    solar_os_gfx_text(gfx, 12, top + 16, title);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char mac_line[96];
    snprintf(mac_line, sizeof(mac_line), "MAC: %02X:%02X:%02X:%02X:%02X:%02X  |  Type: %s  |  RSSI: %ddBm",
             dev->bda[0], dev->bda[1], dev->bda[2], dev->bda[3], dev->bda[4], dev->bda[5],
             dev->addr_type == 0 ? "Public" : "Random", (int)dev->rssi);
    solar_os_gfx_text(gfx, 12, top + 34, mac_line);

    bool is_hid_conn = false;
    solar_os_ble_connected_dev_info_t dev_info;
    const size_t conn_cnt = solar_os_ble_hid_connected_count();
    for (size_t ci = 0; ci < conn_cnt; ci++) {
        if (solar_os_ble_hid_get_connected_dev(ci, &dev_info) && memcmp(dev_info.bda, dev->bda, 6) == 0) {
            is_hid_conn = true;
            break;
        }
    }

    char type_line[96];
    const char *hid_desc = dev->gamepad_like ? "Gamepad / Controller" :
                           dev->mouse_like ? "BLE Mouse (Pointer)" :
                           dev->keyboard_like ? "BLE Keyboard" :
                           (dev->hid_service ? "HID Device" : "None");
    snprintf(type_line, sizeof(type_line), "Appearance: 0x%04X  |  HID: %s  |  Status: %s",
             (unsigned)dev->appearance,
             hid_desc,
             (is_hid_conn || dev->connected) ? "CONNECTED (HID)" :
             ble_state.gatt_connected ? "CONNECTED (GATT)" : (ble_state.gatt_connecting ? "CONNECTING..." : "DISCONNECTED"));
    solar_os_gfx_text(gfx, 12, top + 52, type_line);

    /* 2. Broadcast / Advertising Data Box */
    ble_render_adv_decoded_box(gfx, dev, 6, top + 76, width - 12, 78);

    /* 3. Latest GATT Characteristic Value (if read) */
    solar_os_gfx_rect(gfx, 6, top + 160, width - 12, 80);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 12, top + 176, "GATT Characteristic Live Value:");

    if (ble_state.has_read_value) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        char gatt_hex[96] = "Raw: ";
        for (size_t i = 0; i < ble_state.read_value_len && i < 16; i++) {
            char h[8];
            snprintf(h, sizeof(h), "%02X ", ble_state.read_value[i]);
            strlcat(gatt_hex, h, sizeof(gatt_hex));
        }
        solar_os_gfx_text(gfx, 12, top + 196, gatt_hex);

        char val_summary[96];
        if (ble_state.read_value_len >= 2) {
            uint16_t u16 = (uint16_t)ble_state.read_value[0] | ((uint16_t)ble_state.read_value[1] << 8);
            snprintf(val_summary, sizeof(val_summary), "Handle: 0x%04X  |  Len: %u  |  u16LE: %u",
                     (unsigned)ble_state.last_read_handle, (unsigned)ble_state.read_value_len, (unsigned)u16);
        } else {
            snprintf(val_summary, sizeof(val_summary), "Handle: 0x%04X  |  Len: %u",
                     (unsigned)ble_state.last_read_handle, (unsigned)ble_state.read_value_len);
        }
        solar_os_gfx_text(gfx, 12, top + 214, val_summary);
    } else {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 12, top + 200, "No GATT value read yet. Go to Tab 3 [GATT] and press [R] to read.");
    }
}

/* ---------------------------------------------------------------------
 * View 1: Device Page - Tab 1: Settings & ADV Parser
 * ------------------------------------------------------------------- */

static void ble_draw_tab_settings(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = solar_os_appbar_header_height(gfx) + 6;
    const ble_device_item_t *dev = ble_find_device(ble_state.target_bda);
    if (dev == NULL) return;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    /* 1. Device Alias Settings Box */
    solar_os_gfx_rect(gfx, 6, top, width - 12, 60);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 12, top + 15, "1. Custom Device Alias (Name):");

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char alias_msg[96];
    snprintf(alias_msg, sizeof(alias_msg), "Current: %s  (Original: %s) -> [A] Rename",
             dev->alias[0] ? dev->alias : "(None set)",
             dev->name[0] ? dev->name : "(Unknown)");
    solar_os_gfx_text(gfx, 12, top + 34, alias_msg);

    /* 2. Proximity Alert Settings Box */
    const ble_bookmark_t *bm = ble_find_bookmark(dev->bda);
    solar_os_gfx_rect(gfx, 6, top + 66, width - 12, 46);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 12, top + 80, "2. Proximity Alert:");

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char alert_str[96];
    snprintf(alert_str, sizeof(alert_str), "Alert on Range: %s  (Threshold: %ddBm) -> [P] Toggle",
             (bm != NULL && bm->alert_enabled) ? "ENABLED [BEEP + TOAST]" : "DISABLED",
             (bm != NULL) ? (int)bm->alert_threshold_rssi : -75);
    solar_os_gfx_text(gfx, 12, top + 98, alert_str);

    /* 3. ADV Parser & Lua Configuration Box */
    solar_os_gfx_rect(gfx, 6, top + 118, width - 12, 120);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 12, top + 133, "3. Broadcast Data Decoder Presets & Lua Expression:");

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 12, top + 150, "Presets: [1] Xiaomi Tartisi  |  [2] Standart Baskul  |  [3] Sensor  |  [4] Sifirla");

    char lua_disp[160];
    if (ble_state.custom_lua_expr[0] != '\0') {
        snprintf(lua_disp, sizeof(lua_disp), "Active: %s", ble_state.custom_lua_expr);
    } else {
        snprintf(lua_disp, sizeof(lua_disp), "Active: (Varsayilan Otomatik Cozumleyiciler)");
    }
    solar_os_gfx_text(gfx, 12, top + 172, lua_disp);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 12, top + 198, "[E] Ozel Lua Kodunu Duzenle");

    /* Alias Editing Modal */
    if (ble_state.editing_alias) {
        const int mw = 320;
        const int mh = 80;
        const int mx = (width - mw) / 2;
        const int my = (height - mh) / 2;

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, mx, my, mw, mh);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, mx, my, mw, mh);
        solar_os_gfx_rect(gfx, mx + 2, my + 2, mw - 4, mh - 4);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, mx + 10, my + 20, "Enter Custom Alias:");

        solar_os_gfx_rect(gfx, mx + 10, my + 28, mw - 20, 22);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, mx + 14, my + 44, ble_state.alias_input);

        solar_os_gfx_text(gfx, mx + 10, my + 68, "[Enter] Save Alias   [ESC] Cancel");
    }

    /* Lua Script Editing Modal */
    if (ble_state.editing_lua) {
        const int mw = 360;
        const int mh = 100;
        const int mx = (width - mw) / 2;
        const int my = (height - mh) / 2;

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, mx, my, mw, mh);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, mx, my, mw, mh);
        solar_os_gfx_rect(gfx, mx + 2, my + 2, mw - 4, mh - 4);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, mx + 10, my + 18, "Edit ADV Lua Script Expression:");

        solar_os_gfx_rect(gfx, mx + 8, my + 26, mw - 16, 26);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, mx + 12, my + 44, ble_state.lua_input);

        solar_os_gfx_text(gfx, mx + 10, my + 68, "Available: bytes[1..16] table. Return string.");
        solar_os_gfx_text(gfx, mx + 10, my + 86, "[Enter] Commit Script   [ESC] Cancel");
    }
}

/* ---------------------------------------------------------------------
 * View 1: Device Page - Tab 2: GATT Explorer
 * ------------------------------------------------------------------- */

static void ble_draw_tab_gatt(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = solar_os_appbar_header_height(gfx) + 4;
    const int bottom = height - solar_os_appbar_footer_height(gfx) - 2;

    /* Top GATT Connection Banner */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

    char gatt_hdr[96];
    snprintf(gatt_hdr, sizeof(gatt_hdr), "GATT: %s  |  Services: %u",
             ble_state.gatt_connected ? "CONNECTED (MTU 512)" : (ble_state.gatt_connecting ? "CONNECTING..." : "DISCONNECTED (Press [C] to Connect)"),
             (unsigned)ble_state.service_count);
    solar_os_gfx_text(gfx, 8, top + 12, gatt_hdr);
    solar_os_gfx_line(gfx, 4, top + 18, width - 4, top + 18);

    /* Split Columns: Services (Left) & Characteristics (Right) */
    const int mid_top = top + 22;
    const int mid_h = 135;
    const int split_x = 180;

    /* Left Column: Services */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char srv_hdr[32];
    snprintf(srv_hdr, sizeof(srv_hdr), "Services (%u):", (unsigned)ble_state.service_count);
    solar_os_gfx_text(gfx, 6, mid_top + 10, srv_hdr);

    const int srv_list_y = mid_top + 16;
    const int srv_row_h = 24;
    const int max_srv_rows = (mid_h - 18) / srv_row_h;

    size_t srv_scroll = 0;
    if (ble_state.selected_service >= (size_t)max_srv_rows) {
        srv_scroll = ble_state.selected_service - (size_t)max_srv_rows + 1U;
    }

    for (int r = 0; r < max_srv_rows; r++) {
        const size_t s_idx = srv_scroll + (size_t)r;
        if (s_idx >= ble_state.service_count) break;

        const solar_os_ble_gatt_service_t *srv = &ble_state.services[s_idx];
        const int y = srv_list_y + r * srv_row_h;
        const bool is_sel = (s_idx == ble_state.selected_service);
        const bool has_focus = (ble_state.gatt_focus == GATT_FOCUS_SERVICES);

        if (is_sel) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            if (has_focus) {
                solar_os_gfx_fill_rect(gfx, 4, y, split_x - 8, srv_row_h - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            } else {
                solar_os_gfx_rect(gfx, 4, y, split_x - 8, srv_row_h - 2);
            }
        }

        const char *name = ble_resolve_uuid_name(srv->uuid);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        char s_label[32];
        snprintf(s_label, sizeof(s_label), "%u. %s", (unsigned)(s_idx + 1), name);
        solar_os_gfx_text(gfx, 8, y + 15, s_label);

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    }

    /* Vertical Divider */
    solar_os_gfx_line(gfx, split_x, mid_top, split_x, mid_top + mid_h);

    /* Right Column: Characteristics */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char chr_hdr[32];
    snprintf(chr_hdr, sizeof(chr_hdr), "Characteristics (%u):", (unsigned)ble_state.char_count);
    solar_os_gfx_text(gfx, split_x + 6, mid_top + 10, chr_hdr);

    const int chr_list_y = mid_top + 16;
    const int chr_row_h = 24;
    const int max_chr_rows = (mid_h - 18) / chr_row_h;

    size_t chr_scroll = 0;
    if (ble_state.selected_char >= (size_t)max_chr_rows) {
        chr_scroll = ble_state.selected_char - (size_t)max_chr_rows + 1U;
    }

    for (int r = 0; r < max_chr_rows; r++) {
        const size_t c_idx = chr_scroll + (size_t)r;
        if (c_idx >= ble_state.char_count) break;

        const solar_os_ble_gatt_characteristic_t *ch = &ble_state.characteristics[c_idx];
        const int y = chr_list_y + r * chr_row_h;
        const bool is_sel = (c_idx == ble_state.selected_char);
        const bool has_focus = (ble_state.gatt_focus == GATT_FOCUS_CHARS);

        if (is_sel) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            if (has_focus) {
                solar_os_gfx_fill_rect(gfx, split_x + 4, y, width - split_x - 8, chr_row_h - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            } else {
                solar_os_gfx_rect(gfx, split_x + 4, y, width - split_x - 8, chr_row_h - 2);
            }
        }

        const char *name = ble_resolve_uuid_name(ch->uuid);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        char c_label[32];
        snprintf(c_label, sizeof(c_label), "%s", name);
        solar_os_gfx_text(gfx, split_x + 8, y + 15, c_label);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        char c_props[32];
        snprintf(c_props, sizeof(c_props), "0x%04X (%s)", (unsigned)ch->handle, ch->uuid);
        solar_os_gfx_text(gfx, width - 110, y + 15, c_props);

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    }

    /* Bottom Box: Live Decoded Characteristic Inspector */
    const int box_top = mid_top + mid_h + 4;
    const int box_h = bottom - box_top;
    if (box_h > 20) {
        solar_os_gfx_rect(gfx, 4, box_top, width - 8, box_h);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

        const char *mode_label = ble_state.decoder_mode == GATT_DECODER_AUTO ? "[MODE: AUTO]" :
                                 ble_state.decoder_mode == GATT_DECODER_HEX_ASCII ? "[MODE: HEX & ASCII]" :
                                 ble_state.decoder_mode == GATT_DECODER_NUMERIC ? "[MODE: NUMERIC]" :
                                 "[MODE: LUA EVAL]";
        char title_buf[80];
        snprintf(title_buf, sizeof(title_buf), "Value Inspector %s ([M] Change | [E] Edit Lua):", mode_label);
        solar_os_gfx_text(gfx, 8, box_top + 14, title_buf);

        if (ble_state.has_read_value) {
            if (ble_state.decoder_mode == GATT_DECODER_LUA) {
                if (ble_state.custom_gatt_lua_expr[0] != '\0') {
                    const uint8_t *val = ble_state.read_value;
                    const size_t len = ble_state.read_value_len;
                    char lua_code[300];
                    snprintf(lua_code, sizeof(lua_code),
                             "bytes = {%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u}; handle = %u; len = %u; "
                             "%s",
                             len > 0 ? val[0] : 0, len > 1 ? val[1] : 0, len > 2 ? val[2] : 0, len > 3 ? val[3] : 0,
                             len > 4 ? val[4] : 0, len > 5 ? val[5] : 0, len > 6 ? val[6] : 0, len > 7 ? val[7] : 0,
                             len > 8 ? val[8] : 0, len > 9 ? val[9] : 0, len > 10 ? val[10] : 0, len > 11 ? val[11] : 0,
                             len > 12 ? val[12] : 0, len > 13 ? val[13] : 0, len > 14 ? val[14] : 0, len > 15 ? val[15] : 0,
                             (unsigned)ble_state.last_read_handle, (unsigned)len,
                             ble_state.custom_gatt_lua_expr);
                    char out_buf[96] = "";
                    solar_os_script_run_request_t req = {
                        .input_type = SOLAR_OS_SCRIPT_INPUT_SOURCE,
                        .input = lua_code,
                        .input_len = strlen(lua_code),
                        .source_name = "ble_gatt_eval",
                        .timeout_ms = 500,
                        .output = out_buf,
                        .output_size = sizeof(out_buf),
                    };
                    solar_os_script_run_result_t res = {0};
                    if (solar_os_lua_run(&req, &res) == ESP_OK && res.success && out_buf[0] != '\0') {
                        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
                        solar_os_gfx_text(gfx, 8, box_top + 34, out_buf);
                    } else {
                        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                        solar_os_gfx_text(gfx, 8, box_top + 32, "Lua evaluation error or no return string");
                    }
                } else {
                    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                    solar_os_gfx_text(gfx, 8, box_top + 32, "Lua Mode active. Press [E] to write GATT parser script (or [H] for examples).");
                }
            } else if (ble_state.decoder_mode == GATT_DECODER_HEX_ASCII) {
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                char dump_str[96] = "Hex: ";
                for (size_t i = 0; i < ble_state.read_value_len && i < 12; i++) {
                    char h[8];
                    snprintf(h, sizeof(h), "%02X ", ble_state.read_value[i]);
                    strlcat(dump_str, h, sizeof(dump_str));
                }
                strlcat(dump_str, " | ASCII: ", sizeof(dump_str));
                for (size_t i = 0; i < ble_state.read_value_len && i < 12; i++) {
                    char a[2] = { isprint(ble_state.read_value[i]) ? (char)ble_state.read_value[i] : '.', '\0' };
                    strlcat(dump_str, a, sizeof(dump_str));
                }
                solar_os_gfx_text(gfx, 8, box_top + 32, dump_str);
            } else if (ble_state.decoder_mode == GATT_DECODER_NUMERIC) {
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                char num_str[96];
                uint8_t u8 = ble_state.read_value[0];
                uint16_t u16 = (ble_state.read_value_len >= 2) ? ((uint16_t)ble_state.read_value[0] | ((uint16_t)ble_state.read_value[1] << 8)) : u8;
                snprintf(num_str, sizeof(num_str), "u8: %u | u16LE: %u | hex: 0x%04X | bytes: %u", (unsigned)u8, (unsigned)u16, (unsigned)u16, (unsigned)ble_state.read_value_len);
                solar_os_gfx_text(gfx, 8, box_top + 32, num_str);
            } else {
                /* AUTO */
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                char hex_dump[80] = "Hex: ";
                for (size_t i = 0; i < ble_state.read_value_len && i < 16; i++) {
                    char h[8];
                    snprintf(h, sizeof(h), "%02X ", ble_state.read_value[i]);
                    strlcat(hex_dump, h, sizeof(hex_dump));
                }
                solar_os_gfx_text(gfx, 8, box_top + 32, hex_dump);
            }
        } else {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 8, box_top + 32, "Select characteristic and press [R] or [Space] to read value.");
        }
    }

    /* GATT Lua Script Editing Modal */
    if (ble_state.editing_gatt_lua) {
        const int mw = 360;
        const int mh = 100;
        const int mx = (width - mw) / 2;
        const int my = (height - mh) / 2;

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, mx, my, mw, mh);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, mx, my, mw, mh);
        solar_os_gfx_rect(gfx, mx + 2, my + 2, mw - 4, mh - 4);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, mx + 10, my + 18, "Edit GATT Lua Script Expression:");

        solar_os_gfx_rect(gfx, mx + 8, my + 26, mw - 16, 26);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, mx + 12, my + 44, ble_state.gatt_lua_input);

        solar_os_gfx_text(gfx, mx + 10, my + 68, "Available: bytes[1..16], handle, len. Return string.");
        solar_os_gfx_text(gfx, mx + 10, my + 86, "[Enter] Save Script   [ESC] Cancel");
    }
}

/* ---------------------------------------------------------------------
 * View 1: Device Page - Tab 3: Proximity Radar
 * ------------------------------------------------------------------- */

static void ble_draw_tab_radar(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = solar_os_appbar_header_height(gfx) + 2;
    const ble_device_item_t *dev = ble_find_device(ble_state.target_bda);
    if (dev == NULL) return;

    /* Radar Center & Radius */
    const int cx = 130;
    const int cy = top + 115;
    const int max_radius = 95;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    /* Draw Radar concentric distance rings */
    solar_os_gfx_circle(gfx, cx, cy, max_radius);
    solar_os_gfx_circle(gfx, cx, cy, (max_radius * 2) / 3);
    solar_os_gfx_circle(gfx, cx, cy, max_radius / 3);
    solar_os_gfx_line(gfx, cx - max_radius, cy, cx + max_radius, cy);
    solar_os_gfx_line(gfx, cx, cy - max_radius, cx, cy + max_radius);

    /* Radar sweeping beam */
    const int beam_x = cx + (int)(cosf(ble_state.radar_sweep_angle) * (float)max_radius);
    const int beam_y = cy + (int)(sinf(ble_state.radar_sweep_angle) * (float)max_radius);
    solar_os_gfx_line(gfx, cx, cy, beam_x, beam_y);

    /* Target Device Blip on Radar */
    int8_t r = ble_state.target_rssi;
    if (r == 0) r = dev->rssi;
    if (r > -30) r = -30;
    if (r < -100) r = -100;
    const float dist_pct = (float)(-r - 30) / 70.0f;
    const int blip_r = (int)(dist_pct * (float)(max_radius - 8)) + 6;
    const int blip_x = cx + (int)(cosf(ble_state.radar_sweep_angle - 0.4f) * (float)blip_r);
    const int blip_y = cy + (int)(sinf(ble_state.radar_sweep_angle - 0.4f) * (float)blip_r);

    solar_os_gfx_fill_circle(gfx, blip_x, blip_y, 6);

    /* Right Panel: Signal Statistics & Distance Gauge */
    const int rx = 245;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char t_name[64];
    snprintf(t_name, sizeof(t_name), "%s", dev->alias[0] ? dev->alias : (dev->name[0] ? dev->name : "Target"));
    solar_os_gfx_text(gfx, rx, top + 20, t_name);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char mac_s[48];
    snprintf(mac_s, sizeof(mac_s), "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             dev->bda[0], dev->bda[1], dev->bda[2], dev->bda[3], dev->bda[4], dev->bda[5]);
    solar_os_gfx_text(gfx, rx, top + 38, mac_s);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    char sig_s[32];
    snprintf(sig_s, sizeof(sig_s), "RSSI: %ddBm", (int)ble_state.target_rssi);
    solar_os_gfx_text(gfx, rx, top + 64, sig_s);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const char *prox_str = (ble_state.target_rssi >= -60) ? "PROXIMITY: IMMEDIATE (< 1m)" :
                           (ble_state.target_rssi >= -75) ? "PROXIMITY: NEAR (1 - 3m)" :
                           (ble_state.target_rssi >= -88) ? "PROXIMITY: FAR (3 - 10m)" : "PROXIMITY: VERY FAR (> 10m)";
    solar_os_gfx_text(gfx, rx, top + 86, prox_str);

    char beep_str[48];
    snprintf(beep_str, sizeof(beep_str), "Audio Beeper: %s ([B] Toggle)", ble_state.beep_enabled ? "ON" : "OFF");
    solar_os_gfx_text(gfx, rx, top + 106, beep_str);

    /* Signal Strength History Bar Graph */
    solar_os_gfx_rect(gfx, rx, top + 120, width - rx - 8, 80);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char hist_title[48];
    snprintf(hist_title, sizeof(hist_title), "Signal History (%u):", (unsigned)ble_state.rssi_history_count);
    solar_os_gfx_text(gfx, rx + 4, top + 134, hist_title);

    for (size_t h_idx = 0; h_idx < ble_state.rssi_history_count && h_idx < 24; h_idx++) {
        int8_t hist_r = ble_state.rssi_history[h_idx];
        if (hist_r > -30) hist_r = -30;
        if (hist_r < -100) hist_r = -100;
        int bar_h = (int)((hist_r + 100) * 45 / 70);
        if (bar_h < 2) bar_h = 2;
        int bx = rx + 8 + (int)h_idx * 5;
        int by = top + 195 - bar_h;
        solar_os_gfx_fill_rect(gfx, bx, by, 3, bar_h);
    }
}

/* ---------------------------------------------------------------------
 * Full Scrollable English Help Guide
 * ------------------------------------------------------------------- */

static void ble_draw_help_modal(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = solar_os_appbar_header_height(gfx) + 2;
    const int bottom = height - solar_os_appbar_footer_height(gfx) - 2;
    const int view_h = bottom - top;
    const int line_h = 16;
    const int visible_lines = view_h / line_h;
    const size_t total_lines = sizeof(ble_help_docs) / sizeof(ble_help_docs[0]);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, 2, top, width - 4, view_h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 2, top, width - 4, view_h);

    if (ble_state.help_scroll_line + visible_lines > total_lines && total_lines > (size_t)visible_lines) {
        ble_state.help_scroll_line = total_lines - (size_t)visible_lines;
    }

    for (int i = 0; i < visible_lines; i++) {
        const size_t line_idx = ble_state.help_scroll_line + (size_t)i;
        if (line_idx >= total_lines) break;

        const ble_help_line_t *item = &ble_help_docs[line_idx];
        const int y = top + 4 + i * line_h;

        if (item->is_header) {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, 10, y + 12, item->text);
        } else if (item->is_code) {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, 18, y + 12, item->text);
        } else {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 10, y + 12, item->text);
        }
    }

    /* Scroll Bar on Right Edge */
    if (total_lines > (size_t)visible_lines) {
        const int bar_x = width - 10;
        const int bar_w = 4;
        const int bar_h_total = view_h - 10;
        const int thumb_h = (bar_h_total * visible_lines) / (int)total_lines;
        const int thumb_y = top + 5 + (int)((ble_state.help_scroll_line * (size_t)(bar_h_total - thumb_h)) / (total_lines - (size_t)visible_lines));

        solar_os_gfx_rect(gfx, bar_x, top + 5, bar_w, bar_h_total);
        solar_os_gfx_fill_rect(gfx, bar_x, thumb_y, bar_w, thumb_h > 8 ? thumb_h : 8);
    }
}

static void ble_draw_paired_modal(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = solar_os_appbar_header_height(gfx) + 2;
    const int bottom = height - solar_os_appbar_footer_height(gfx) - 2;
    const int view_h = bottom - top;
    const int row_h = 24;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, 2, top, width - 4, view_h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 2, top, width - 4, view_h);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 10, top + 16, "PAIRED DEVICES");
    solar_os_gfx_line(gfx, 4, top + 22, width - 6, top + 22);

    const size_t count = solar_os_ble_hid_remembered_count();
    if (count == 0) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 10, top + 46, "No paired devices yet.");
        solar_os_gfx_text(gfx, 10, top + 62, "Pair one with [P] on a device's Overview tab.");
        return;
    }

    if (ble_state.paired_selected >= count) {
        ble_state.paired_selected = count - 1;
    }

    const int list_top = top + 28;
    const int visible_rows = (bottom - list_top) / row_h;
    size_t scroll = 0;
    if (visible_rows > 0 && ble_state.paired_selected >= (size_t)visible_rows) {
        scroll = ble_state.paired_selected - (size_t)visible_rows + 1;
    }

    for (int i = 0; i < visible_rows; i++) {
        const size_t idx = scroll + (size_t)i;
        if (idx >= count) break;

        solar_os_ble_hid_remembered_info_t info;
        if (!solar_os_ble_hid_get_remembered(idx, &info)) break;

        const int row_y = list_top + i * row_h;
        const bool selected = idx == ble_state.paired_selected;

        if (selected) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 4, row_y, width - 8, row_h - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        const char *label = info.name[0] != '\0' ? info.name : "Unnamed Device";
        solar_os_gfx_text(gfx, 12, row_y + 15, label);

        char addr_buf[24];
        snprintf(addr_buf, sizeof(addr_buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 info.bda[0], info.bda[1], info.bda[2], info.bda[3], info.bda[4], info.bda[5]);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, width - 190, row_y + 15, addr_buf);

        solar_os_gfx_text(gfx, width - 60, row_y + 15, info.connected ? "LINK" : "-");
    }
}

/* ---------------------------------------------------------------------
 * Master Render Routine
 * ------------------------------------------------------------------- */

static void ble_scanner_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    ble_draw_header(gfx);

    if (ble_state.view == BLE_VIEW_SCANNER) {
        ble_draw_scanner_list(gfx, width, height);
    } else {
        switch (ble_state.active_tab) {
        case DEV_TAB_OVERVIEW:
            ble_draw_tab_overview(gfx, width, height);
            break;
        case DEV_TAB_SETTINGS:
            ble_draw_tab_settings(gfx, width, height);
            break;
        case DEV_TAB_GATT:
            ble_draw_tab_gatt(gfx, width, height);
            break;
        case DEV_TAB_RADAR:
            ble_draw_tab_radar(gfx, width, height);
            break;
        default:
            break;
        }
    }

    if (ble_state.showing_help) {
        ble_draw_help_modal(gfx, width, height);
    } else if (ble_state.showing_paired) {
        ble_draw_paired_modal(gfx, width, height);
    }

    ble_draw_footer(gfx, width, height);
    solar_os_gfx_present(gfx);
    ble_state.render_pending = false;
}

/* ---------------------------------------------------------------------
 * App Lifecycle & Event Handling
 * ------------------------------------------------------------------- */

static void ble_open_device_page(const ble_device_item_t *dev)
{
    if (dev == NULL) return;

    memcpy(ble_state.target_bda, dev->bda, 6);
    ble_state.target_addr_type = dev->addr_type;
    ble_state.target_rssi = dev->rssi;
    strlcpy(ble_state.target_name, dev->name[0] ? dev->name : "(Unnamed)", sizeof(ble_state.target_name));
    strlcpy(ble_state.target_alias, dev->alias, sizeof(ble_state.target_alias));

    ble_state.view = BLE_VIEW_DEVICE_PAGE;
    ble_state.active_tab = DEV_TAB_OVERVIEW;

    /* Initialize history for immediate radar display */
    ble_state.rssi_history_count = 0;
    if (dev->rssi != 0) {
        ble_state.rssi_history[ble_state.rssi_history_count++] = dev->rssi;
    }

    ble_state.render_pending = true;
}

static void ble_scanner_handle_char(solar_os_context_t *ctx, char ch)
{
    const unsigned char uch = (unsigned char)ch;

    /* Help Modal Scroll & Close */
    if (ble_state.showing_help) {
        const size_t total = sizeof(ble_help_docs) / sizeof(ble_help_docs[0]);
        if (uch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
            if (ble_state.help_scroll_line > 0) {
                ble_state.help_scroll_line--;
                ble_state.render_pending = true;
            }
            return;
        }
        if (uch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
            if (ble_state.help_scroll_line + 14 < total) {
                ble_state.help_scroll_line++;
                ble_state.render_pending = true;
            }
            return;
        }
        if (uch == SOLAR_OS_KEY_ESCAPE || ch == 'h' || ch == 'H' || ch == 'q' || ch == 'Q') {
            ble_state.showing_help = false;
            ble_state.help_scroll_line = 0;
            ble_state.render_pending = true;
            return;
        }
        return;
    }

    /* Paired Devices Modal: Navigate / Reconnect / Forget / Close */
    if (ble_state.showing_paired) {
        const size_t count = solar_os_ble_hid_remembered_count();
        if (uch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
            if (ble_state.paired_selected > 0) {
                ble_state.paired_selected--;
                ble_state.render_pending = true;
            }
            return;
        }
        if (uch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
            if (count > 0 && ble_state.paired_selected + 1 < count) {
                ble_state.paired_selected++;
                ble_state.render_pending = true;
            }
            return;
        }
        if (ch == '\n' || ch == '\r') {
            solar_os_ble_hid_remembered_info_t info;
            if (solar_os_ble_hid_get_remembered(ble_state.paired_selected, &info)) {
                if (info.connected) {
                    ble_scanner_set_status("Already connected");
                } else {
                    const esp_err_t err = solar_os_ble_hid_connect(info.bda, info.addr_type, info.name);
                    ble_scanner_set_status(err == ESP_OK ? "Reconnecting..." : "Reconnect request failed");
                }
                ble_state.render_pending = true;
            }
            return;
        }
        if (ch == 'd' || ch == 'D' || uch == 0x7fU) {
            solar_os_ble_hid_remembered_info_t info;
            if (solar_os_ble_hid_get_remembered(ble_state.paired_selected, &info)) {
                if (info.connected) {
                    (void)solar_os_ble_hid_disconnect(info.bda);
                }
                (void)solar_os_ble_hid_forget(info.bda);
                ble_scanner_set_status("Forgotten");
                if (ble_state.paired_selected > 0) {
                    ble_state.paired_selected--;
                }
                ble_state.render_pending = true;
            }
            return;
        }
        if (uch == SOLAR_OS_KEY_ESCAPE || ch == 'm' || ch == 'M' || ch == 'q' || ch == 'Q') {
            ble_state.showing_paired = false;
            ble_state.render_pending = true;
            return;
        }
        return;
    }

    /* Help Toggle */
    if ((ch == 'h' || ch == 'H' || ch == '?') &&
        !ble_state.editing_alias && !ble_state.editing_lua && !ble_state.editing_gatt_lua) {
        ble_state.showing_help = true;
        ble_state.help_scroll_line = 0;
        ble_state.render_pending = true;
        return;
    }

    /* Alias Editing Modal Keys */
    if (ble_state.editing_alias) {
        if (ch == '\n' || ch == '\r') {
            ble_state.editing_alias = false;
            ble_bookmark_t *bm = ble_find_bookmark(ble_state.target_bda);
            if (bm != NULL) {
                strlcpy(bm->alias, ble_state.alias_input, sizeof(bm->alias));
            } else if (ble_state.bookmark_count < BLE_SCANNER_MAX_BOOKMARKS) {
                bm = &ble_state.bookmarks[ble_state.bookmark_count++];
                memcpy(bm->bda, ble_state.target_bda, 6);
                strlcpy(bm->alias, ble_state.alias_input, sizeof(bm->alias));
                bm->alert_enabled = false;
                bm->alert_threshold_rssi = -75;
            }
            ble_scanner_save_bookmarks();
            ble_sync_device_bookmarks();
            strlcpy(ble_state.target_alias, ble_state.alias_input, sizeof(ble_state.target_alias));
            ble_scanner_set_status("Alias saved!");
            ble_state.render_pending = true;
        } else if (uch == SOLAR_OS_KEY_ESCAPE) {
            ble_state.editing_alias = false;
            ble_state.render_pending = true;
        } else if (ch == '\b' || uch == 0x7fU || uch == 0x08U) {
            if (ble_state.alias_cursor > 0) {
                ble_state.alias_input[--ble_state.alias_cursor] = '\0';
                ble_state.render_pending = true;
            }
        } else if (ch >= 32 && ch <= 126 && ble_state.alias_cursor + 1 < sizeof(ble_state.alias_input)) {
            ble_state.alias_input[ble_state.alias_cursor++] = ch;
            ble_state.alias_input[ble_state.alias_cursor] = '\0';
            ble_state.render_pending = true;
        }
        return;
    }

    /* ADV Lua Script Editing Modal Keys */
    if (ble_state.editing_lua) {
        if (ch == '\n' || ch == '\r') {
            ble_state.editing_lua = false;
            strlcpy(ble_state.custom_lua_expr, ble_state.lua_input, sizeof(ble_state.custom_lua_expr));
            ble_scanner_set_status("ADV Lua script updated!");
            ble_state.render_pending = true;
        } else if (uch == SOLAR_OS_KEY_ESCAPE) {
            ble_state.editing_lua = false;
            ble_state.render_pending = true;
        } else if (ch == '\b' || uch == 0x7fU || uch == 0x08U) {
            if (ble_state.lua_cursor > 0) {
                ble_state.lua_input[--ble_state.lua_cursor] = '\0';
                ble_state.render_pending = true;
            }
        } else if (ch >= 32 && ch <= 126 && ble_state.lua_cursor + 1 < sizeof(ble_state.lua_input)) {
            ble_state.lua_input[ble_state.lua_cursor++] = ch;
            ble_state.lua_input[ble_state.lua_cursor] = '\0';
            ble_state.render_pending = true;
        }
        return;
    }

    /* GATT Lua Script Editing Modal Keys */
    if (ble_state.editing_gatt_lua) {
        if (ch == '\n' || ch == '\r') {
            ble_state.editing_gatt_lua = false;
            strlcpy(ble_state.custom_gatt_lua_expr, ble_state.gatt_lua_input, sizeof(ble_state.custom_gatt_lua_expr));
            ble_state.decoder_mode = GATT_DECODER_LUA;
            ble_scanner_set_status("GATT Lua script updated!");
            ble_state.render_pending = true;
        } else if (uch == SOLAR_OS_KEY_ESCAPE) {
            ble_state.editing_gatt_lua = false;
            ble_state.render_pending = true;
        } else if (ch == '\b' || uch == 0x7fU || uch == 0x08U) {
            if (ble_state.gatt_lua_cursor > 0) {
                ble_state.gatt_lua_input[--ble_state.gatt_lua_cursor] = '\0';
                ble_state.render_pending = true;
            }
        } else if (ch >= 32 && ch <= 126 && ble_state.gatt_lua_cursor + 1 < sizeof(ble_state.gatt_lua_input)) {
            ble_state.gatt_lua_input[ble_state.gatt_lua_cursor++] = ch;
            ble_state.gatt_lua_input[ble_state.gatt_lua_cursor] = '\0';
            ble_state.render_pending = true;
        }
        return;
    }

    /* -------------------------------------------------------------
     * View 0: Main Scanner List Events
     * ----------------------------------------------------------- */
    if (ble_state.view == BLE_VIEW_SCANNER) {
        const size_t count = ble_filtered_device_count();
        if (uch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W' ||
            uch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A') {
            if (ble_state.selected_device > 0) {
                ble_state.selected_device--;
                ble_state.render_pending = true;
            }
            return;
        }
        if (uch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S' ||
            uch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D') {
            if (count > 0 && ble_state.selected_device + 1 < count) {
                ble_state.selected_device++;
                ble_state.render_pending = true;
            }
            return;
        }
        if (ch == '\n' || ch == '\r') {
            const ble_device_item_t *dev = ble_get_filtered_device(ble_state.selected_device);
            if (dev != NULL) {
                ble_open_device_page(dev);
            }
            return;
        }
        if (ch == ' ' || ch == 'r' || ch == 'R') {
            ble_scanner_start_scan();
            ble_scanner_set_status("Scanning...");
            return;
        }
        if (ch == 'f' || ch == 'F') {
            ble_state.filter = (ble_filter_mode_t)((ble_state.filter + 1) % BLE_FILTER_COUNT);
            ble_state.selected_device = 0;
            ble_state.render_pending = true;
            return;
        }
        if (ch == 'm' || ch == 'M') {
            ble_state.showing_paired = true;
            ble_state.paired_selected = 0;
            ble_state.render_pending = true;
            return;
        }
        if (uch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return;
        }
        return;
    }

    /* -------------------------------------------------------------
     * View 1: Device Details Page (Multi-Tab) Events
     * ----------------------------------------------------------- */
    if (ble_state.view == BLE_VIEW_DEVICE_PAGE) {
        /* Tab Navigation via [Tab] */
        if (ch == '\t') {
            ble_state.active_tab = (dev_tab_t)((ble_state.active_tab + 1) % DEV_TAB_COUNT);
            if (ble_state.active_tab == DEV_TAB_RADAR) {
                ble_state.next_auto_rescan_ms = 0; /* Scan radar immediately */
                ble_scanner_start_scan();
            }
            ble_state.render_pending = true;
            return;
        }

        /* Escape back to scanner view */
        if (uch == SOLAR_OS_KEY_ESCAPE || uch == 0x08U || uch == 0x7fU) {
            ble_state.view = BLE_VIEW_SCANNER;
            (void)solar_os_ble_gatt_disconnect();
            ble_state.gatt_connected = false;
            ble_state.render_pending = true;
            return;
        }

        /* Tab Specific Event Handlers */
        switch (ble_state.active_tab) {
        case DEV_TAB_OVERVIEW:
            if (ch == '1') {
                ble_state.active_tab = DEV_TAB_OVERVIEW;
                ble_state.render_pending = true;
            } else if (ch == '2') {
                ble_state.active_tab = DEV_TAB_SETTINGS;
                ble_state.render_pending = true;
            } else if (ch == '3') {
                ble_state.active_tab = DEV_TAB_GATT;
                ble_state.render_pending = true;
            } else if (ch == '4') {
                ble_state.active_tab = DEV_TAB_RADAR;
                ble_state.next_auto_rescan_ms = 0;
                ble_scanner_start_scan();
                ble_state.render_pending = true;
            } else if (ch == 'p' || ch == 'P' || ch == '\n' || ch == '\r') {
                /* Enter also pairs here (not just [P]) so a keyboard-less
                 * session -- e.g. only the on-screen BLE keyboard bound at
                 * the main menu -- has a way to pair a second device
                 * without depending on that first link staying up. */
                const ble_device_item_t *d = ble_find_device(ble_state.target_bda);
                if (d != NULL) {
                    if (ble_state.gatt_connected || ble_state.gatt_connecting) {
                        (void)solar_os_ble_gatt_disconnect();
                        ble_state.gatt_connected = false;
                        ble_state.gatt_connecting = false;
                    }
                    const esp_err_t err = solar_os_ble_hid_connect(d->bda, d->addr_type, d->name);
                    if (err == ESP_OK) {
                        ble_scanner_set_status("Connecting / Pairing BLE HID...");
                    } else {
                        ble_scanner_set_status("HID Connect request failed");
                    }
                }
                ble_state.render_pending = true;
            } else if (ch == 'd' || ch == 'D') {
                const ble_device_item_t *d = ble_find_device(ble_state.target_bda);
                if (d != NULL) {
                    const esp_err_t err = solar_os_ble_hid_disconnect(d->bda);
                    if (err == ESP_OK) {
                        ble_scanner_set_status("HID Disconnected");
                    } else {
                        ble_scanner_set_status("Device not connected");
                    }
                }
                ble_state.render_pending = true;
            }
            break;

        case DEV_TAB_SETTINGS:
            if (ch == '1') {
                strlcpy(ble_state.custom_lua_expr, "return string.format('Xiaomi Scale: %.2f kg', ((bytes[12] or 0)*256 + (bytes[11] or 0))/200)", sizeof(ble_state.custom_lua_expr));
                ble_scanner_set_status("Preset 1 (Xiaomi Scale) applied");
                ble_state.render_pending = true;
            } else if (ch == '2') {
                strlcpy(ble_state.custom_lua_expr, "return string.format('Standard Scale: %.2f kg', ((bytes[5] or 0)*256 + (bytes[4] or 0))*0.005)", sizeof(ble_state.custom_lua_expr));
                ble_scanner_set_status("Preset 2 (Standard Scale 0x181D) applied");
                ble_state.render_pending = true;
            } else if (ch == '3') {
                strlcpy(ble_state.custom_lua_expr, "return 'Sensor: ' .. (bytes[3] or 0) .. ' C, Hum: ' .. (bytes[4] or 0) .. '%'", sizeof(ble_state.custom_lua_expr));
                ble_scanner_set_status("Preset 3 (Sensor) applied");
                ble_state.render_pending = true;
            } else if (ch == '4') {
                ble_state.custom_lua_expr[0] = '\0';
                ble_scanner_set_status("Preset 4 (Default Decoders) restored");
                ble_state.render_pending = true;
            } else if (ch == 'a' || ch == 'A') {
                ble_state.editing_alias = true;
                strlcpy(ble_state.alias_input, ble_state.target_alias[0] ? ble_state.target_alias : ble_state.target_name, sizeof(ble_state.alias_input));
                ble_state.alias_cursor = strlen(ble_state.alias_input);
                ble_state.render_pending = true;
            } else if (ch == 'p' || ch == 'P') {
                ble_bookmark_t *bm = ble_find_bookmark(ble_state.target_bda);
                if (bm != NULL) {
                    bm->alert_enabled = !bm->alert_enabled;
                } else if (ble_state.bookmark_count < BLE_SCANNER_MAX_BOOKMARKS) {
                    bm = &ble_state.bookmarks[ble_state.bookmark_count++];
                    memcpy(bm->bda, ble_state.target_bda, 6);
                    strlcpy(bm->alias, ble_state.target_alias[0] ? ble_state.target_alias : ble_state.target_name, sizeof(bm->alias));
                    bm->alert_enabled = true;
                    bm->alert_threshold_rssi = -75;
                }
                ble_scanner_save_bookmarks();
                ble_scanner_set_status((bm != NULL && bm->alert_enabled) ? "Proximity Alert ENABLED" : "Proximity Alert DISABLED");
                ble_state.render_pending = true;
            } else if (ch == 'e' || ch == 'E') {
                ble_state.editing_lua = true;
                strlcpy(ble_state.lua_input, ble_state.custom_lua_expr, sizeof(ble_state.lua_input));
                ble_state.lua_cursor = strlen(ble_state.lua_input);
                ble_state.render_pending = true;
            }
            break;

        case DEV_TAB_GATT:
            if (ch == 'c' || ch == 'C' || ch == '\n' || ch == '\r') {
                if (ble_state.gatt_connected) {
                    (void)solar_os_ble_gatt_disconnect();
                    ble_state.gatt_connected = false;
                    ble_state.service_count = 0;
                    ble_state.char_count = 0;
                    ble_scanner_set_status("GATT Disconnected");
                } else {
                    ble_start_device_gatt_connect();
                }
                ble_state.render_pending = true;
            } else if (uch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A') {
                ble_state.gatt_focus = GATT_FOCUS_SERVICES;
                ble_state.render_pending = true;
            } else if (uch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D') {
                ble_state.gatt_focus = GATT_FOCUS_CHARS;
                ble_state.render_pending = true;
            } else if (uch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
                if (ble_state.gatt_focus == GATT_FOCUS_SERVICES && ble_state.selected_service > 0) {
                    ble_state.selected_service--;
                    ble_load_service_characteristics(ble_state.selected_service);
                } else if (ble_state.gatt_focus == GATT_FOCUS_CHARS && ble_state.selected_char > 0) {
                    ble_state.selected_char--;
                    ble_state.render_pending = true;
                }
            } else if (uch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
                if (ble_state.gatt_focus == GATT_FOCUS_SERVICES && ble_state.selected_service + 1 < ble_state.service_count) {
                    ble_state.selected_service++;
                    ble_load_service_characteristics(ble_state.selected_service);
                } else if (ble_state.gatt_focus == GATT_FOCUS_CHARS && ble_state.selected_char + 1 < ble_state.char_count) {
                    ble_state.selected_char++;
                    ble_state.render_pending = true;
                }
            } else if (ch == 'r' || ch == 'R' || ch == ' ') {
                ble_read_selected_characteristic();
            } else if (ch == 'm' || ch == 'M') {
                ble_state.decoder_mode = (gatt_decoder_mode_t)((ble_state.decoder_mode + 1) % GATT_DECODER_COUNT);
                ble_state.render_pending = true;
            } else if (ch == 'e' || ch == 'E') {
                ble_state.editing_gatt_lua = true;
                strlcpy(ble_state.gatt_lua_input, ble_state.custom_gatt_lua_expr, sizeof(ble_state.gatt_lua_input));
                ble_state.gatt_lua_cursor = strlen(ble_state.gatt_lua_input);
                ble_state.render_pending = true;
            }
            break;

        case DEV_TAB_RADAR:
            if (ch == 'b' || ch == 'B') {
                ble_state.beep_enabled = !ble_state.beep_enabled;
                ble_scanner_set_status(ble_state.beep_enabled ? "Radar Beeper ON" : "Radar Beeper OFF");
                ble_state.render_pending = true;
            } else if (ch == 'r' || ch == 'R' || ch == ' ') {
                ble_scanner_start_scan();
                ble_scanner_set_status("Tracking target...");
            }
            break;

        default:
            break;
        }
        return;
    }
}

static bool ble_scanner_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return true;

    switch (event->type) {
    case SOLAR_OS_EVENT_CHAR:
        ble_scanner_handle_char(ctx, event->data.ch);
        break;

    case SOLAR_OS_EVENT_SCROLL:
        if (ble_state.view == BLE_VIEW_SCANNER && !ble_state.showing_paired && !ble_state.showing_help) {
            const size_t count = ble_filtered_device_count();
            const bool down = event->data.scroll.delta < 0;
            if (down) {
                if (count > 0 && ble_state.selected_device + 1 < count) {
                    ble_state.selected_device++;
                    ble_state.render_pending = true;
                }
            } else if (ble_state.selected_device > 0) {
                ble_state.selected_device--;
                ble_state.render_pending = true;
            }
            if (ble_state.render_pending) ble_scanner_render(ctx);
        }
        break;

    case SOLAR_OS_EVENT_TICK:
        ble_state.elapsed_ms += BLE_SCANNER_TICK_MS;

        /* Process scan staging results */
        if (ble_state.task_done) {
            bool ready = false;
            portENTER_CRITICAL(&ble_scanner_lock);
            ready = ble_state.staging_ready;
            ble_state.task_done = false;
            ble_state.staging_ready = false;
            ble_state.scanning = false;
            portEXIT_CRITICAL(&ble_scanner_lock);

            if (ready) {
                solar_os_ble_keyboard_scan_result_t staged[BLE_SCANNER_MAX_DEVICES];
                size_t count = 0U;

                portENTER_CRITICAL(&ble_scanner_lock);
                count = ble_state.staging_count;
                if (count > 0) {
                    memcpy(staged, ble_state.staging_results, count * sizeof(staged[0]));
                }
                portEXIT_CRITICAL(&ble_scanner_lock);

                for (size_t s = 0; s < count; s++) {
                    ble_device_item_t *target = NULL;
                    for (size_t d = 0; d < ble_state.device_count; d++) {
                        if (memcmp(ble_state.devices[d].bda, staged[s].bda, 6) == 0) {
                            target = &ble_state.devices[d];
                            break;
                        }
                    }
                    if (target == NULL && ble_state.device_count < BLE_SCANNER_MAX_DEVICES) {
                        target = &ble_state.devices[ble_state.device_count++];
                        memset(target, 0, sizeof(*target));
                        memcpy(target->bda, staged[s].bda, 6);
                    }
                    if (target == NULL) continue;

                    target->addr_type = staged[s].addr_type;
                    target->rssi = staged[s].rssi;
                    target->tx_power = staged[s].tx_power;
                    target->appearance = staged[s].appearance;
                    target->hid_service = staged[s].hid_service;
                    target->keyboard_like = staged[s].keyboard_like;
                    target->mouse_like = staged[s].mouse_like;
                    target->gamepad_like = staged[s].gamepad_like;
                    target->remembered = staged[s].remembered;
                    target->connected = staged[s].connected;
                    target->last_seen_ms = ble_state.elapsed_ms;

                    if (staged[s].name[0] != '\0') {
                        strlcpy(target->name, staged[s].name, sizeof(target->name));
                    }
                    if (staged[s].adv_data_len > 0) {
                        memcpy(target->adv_data, staged[s].adv_data, staged[s].adv_data_len);
                        target->adv_data_len = staged[s].adv_data_len;
                    }

                    /* Proximity Alerts */
                    const ble_bookmark_t *bm = ble_find_bookmark(target->bda);
                    if (bm != NULL && bm->alert_enabled) {
                        const bool in_range_now = (target->rssi >= bm->alert_threshold_rssi);
                        if (in_range_now && !target->is_in_range) {
                            target->is_in_range = true;
                            (void)solar_os_audio_play_tone(1500, 60, 45);
                            char alert_msg[80];
                            snprintf(alert_msg, sizeof(alert_msg), "[ALERT] '%s' is IN RANGE (%ddBm)!",
                                     bm->alias, (int)target->rssi);
                            ble_scanner_set_status(alert_msg);
                        } else if (!in_range_now && target->is_in_range) {
                            target->is_in_range = false;
                            (void)solar_os_audio_play_tone(800, 80, 45);
                            char alert_msg[80];
                            snprintf(alert_msg, sizeof(alert_msg), "[ALERT] '%s' is OUT OF RANGE!", bm->alias);
                            ble_scanner_set_status(alert_msg);
                        }
                    }

                    /* Target radar tracking history update */
                    if (memcmp(target->bda, ble_state.target_bda, 6) == 0) {
                        ble_state.target_rssi = target->rssi;
                        if (ble_state.rssi_history_count < sizeof(ble_state.rssi_history)) {
                            ble_state.rssi_history[ble_state.rssi_history_count++] = target->rssi;
                        } else {
                            memmove(&ble_state.rssi_history[0], &ble_state.rssi_history[1], sizeof(ble_state.rssi_history) - 1);
                            ble_state.rssi_history[sizeof(ble_state.rssi_history) - 1] = target->rssi;
                        }
                    }
                }

                ble_sync_device_bookmarks();

                char msg[64];
                snprintf(msg, sizeof(msg), "Scan updated: %u devices found", (unsigned)ble_state.device_count);
                ble_scanner_set_status(msg);
                ble_state.render_pending = true;
            }
        }

        /* Periodic continuous auto-scan: 60s for List, 3s for Radar */
        if (!ble_state.scanning && !ble_state.gatt_connecting && !ble_state.gatt_connected) {
            const uint32_t auto_interval = (ble_state.view == BLE_VIEW_DEVICE_PAGE && ble_state.active_tab == DEV_TAB_RADAR) ?
                                           BLE_SCANNER_RADAR_RESCAN_MS : BLE_SCANNER_LIST_AUTORESCAN_MS;
            if (ble_state.elapsed_ms >= ble_state.next_auto_rescan_ms) {
                ble_state.next_auto_rescan_ms = ble_state.elapsed_ms + auto_interval;
                ble_scanner_start_scan();
            }
        }

        /* Radar animations & beeps */
        if (ble_state.view == BLE_VIEW_DEVICE_PAGE && ble_state.active_tab == DEV_TAB_RADAR) {
            ble_state.radar_sweep_angle += 0.15f;
            if (ble_state.radar_sweep_angle > 6.283185f) {
                ble_state.radar_sweep_angle = 0.0f;
            }
            if (ble_state.beep_enabled && ble_state.elapsed_ms >= ble_state.next_beep_ms) {
                int8_t r = ble_state.target_rssi;
                if (r > -30) r = -30;
                if (r < -95) r = -95;
                const uint32_t interval = (uint32_t)(100 + (-r - 30) * 12);
                ble_state.next_beep_ms = ble_state.elapsed_ms + interval;
                (void)solar_os_audio_play_tone(1800, 30, 40);
            }
            ble_state.render_pending = true;
        }

        if (ble_state.status_until_ms != 0U && ble_state.status_until_ms <= ble_state.elapsed_ms &&
            ble_state.status_until_ms + BLE_SCANNER_TICK_MS > ble_state.elapsed_ms) {
            ble_state.render_pending = true;
        }

        if (ble_state.render_pending) {
            ble_scanner_render(ctx);
        }
        break;

    case SOLAR_OS_EVENT_RESUME:
        ble_state.render_pending = true;
        ble_scanner_render(ctx);
        break;

    case SOLAR_OS_EVENT_CLICK: {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) break;

        char status_buf[64];
        solar_os_appbar_header_t header;
        ble_build_header(&header, status_buf, sizeof(status_buf));

        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, event->data.click.x, event->data.click.y, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                ble_scanner_handle_char(ctx, (char)SOLAR_OS_KEY_ESCAPE);
            } else if (hit.kind == SOLAR_OS_APPBAR_HIT_TAB_ITEM && hit.index < DEV_TAB_COUNT) {
                ble_state.active_tab = (dev_tab_t)hit.index;
                if (ble_state.active_tab == DEV_TAB_RADAR) {
                    ble_state.next_auto_rescan_ms = 0;
                    ble_scanner_start_scan();
                }
                ble_state.render_pending = true;
            }
            if (ble_state.render_pending) ble_scanner_render(ctx);
            break;
        }

        if (ble_state.view == BLE_VIEW_SCANNER && !ble_state.showing_paired) {
            const int width = (int)solar_os_gfx_width(gfx);
            const int height = (int)solar_os_gfx_height(gfx);
            size_t dev_idx;
            if (ble_scanner_hit_test_list(gfx, width, height, event->data.click.x, event->data.click.y, &dev_idx)) {
                ble_state.selected_device = dev_idx;
                const ble_device_item_t *dev = ble_get_filtered_device(dev_idx);
                if (dev != NULL) {
                    ble_open_device_page(dev);
                }
                ble_scanner_render(ctx);
                break;
            }
        }

        const bool showing_status = ble_state.status_until_ms > ble_state.elapsed_ms &&
                                    ble_state.status_message[0] != '\0' && !ble_state.showing_paired;
        if (!showing_status) {
            solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
            const size_t count = ble_build_footer_shortcuts(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
            const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };

            solar_os_appbar_hit_t fhit;
            if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, event->data.click.x, event->data.click.y, &fhit) &&
                fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM) {
                ble_scanner_handle_char(ctx, items[fhit.index].key);
                if (ble_state.render_pending) ble_scanner_render(ctx);
            }
        }
        break;
    }

    default:
        break;
    }
    return true;
}

static esp_err_t ble_scanner_start(solar_os_context_t *ctx)
{
    memset(&ble_state, 0, sizeof(ble_state));
    ble_state.filter = BLE_FILTER_ALL;
    ble_state.view = BLE_VIEW_SCANNER;
    ble_state.active_tab = DEV_TAB_OVERVIEW;
    ble_state.adv_mode = ADV_INSPECT_AUTO;
    ble_state.beep_enabled = true;
    ble_state.next_auto_rescan_ms = 1000;
    ble_state.render_pending = true;

    ble_scanner_load_bookmarks();

    solar_os_context_set_graphics_active(ctx, true);
    ble_scanner_start_scan();
    ble_scanner_render(ctx);
    return ESP_OK;
}

static void ble_scanner_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    ble_scanner_stop_worker();
    (void)solar_os_ble_gatt_disconnect();
}

static void ble_scanner_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    snprintf(buffer, buffer_len, "BLE: %u Devices", (unsigned)ble_state.device_count);
}

const solar_os_app_t solar_os_ble_scanner_app = {
    .name = "ble",
    .summary = "Bluetooth Low Energy scanner and GATT service explorer",
    .flags = 0,
    .start = ble_scanner_start,
    .stop = ble_scanner_stop,
    .event = ble_scanner_event,
    .title = ble_scanner_title,
    .state_slot = &ble_scanner_state_ptr,
    .state_size = sizeof(ble_scanner_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = BLE_SCANNER_TASK_STACK,
    .worker_stack_external = false,
    .tick_interval_ms = BLE_SCANNER_TICK_MS,
};
