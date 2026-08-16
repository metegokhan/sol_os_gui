#include "solar_os_ble_scanner.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "solar_os_audio.h"
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
#define BLE_SCANNER_LIST_AUTORESCAN_MS 60000U /* 60s for low overhead list rescan */
#define BLE_SCANNER_RADAR_RESCAN_MS 5000U     /* 5s for target radar tracking */

#define BLE_HEADER_H 24
#define BLE_FOOTER_H 22

typedef enum {
    BLE_VIEW_SCANNER = 0,
    BLE_VIEW_INTEGRATED_GATT = 1,
    BLE_VIEW_RADAR = 2,
    BLE_VIEW_ADV_INSPECTOR = 3,
} ble_view_mode_t;

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
    uint32_t next_auto_rescan_ms;

    /* Bookmarks / Saved Devices */
    ble_bookmark_t bookmarks[BLE_SCANNER_MAX_BOOKMARKS];
    size_t bookmark_count;

    /* Alias editing modal */
    bool editing_alias;
    char alias_input[32];
    size_t alias_cursor;

    /* --- ADV Inspector State --- */
    adv_inspect_mode_t adv_mode;
    char custom_lua_expr[160];
    bool editing_lua;
    char lua_input[160];
    size_t lua_cursor;

    /* --- Integrated GATT Explorer State --- */
    bool gatt_connecting;
    bool gatt_connected;
    uint8_t connected_bda[6];
    uint8_t connected_addr_type;
    char connected_name[SOLAR_OS_BLE_KEYBOARD_NAME_MAX];
    char connected_alias[32];
    int8_t connected_rssi;
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

    /* --- Radar / Tracking State --- */
    uint8_t tracked_bda[6];
    char tracked_name[SOLAR_OS_BLE_KEYBOARD_NAME_MAX];
    int8_t tracked_rssi;
    bool tracked_found;
    int8_t rssi_history[32];
    size_t rssi_history_count;
    uint32_t next_beep_ms;
    bool beep_enabled;
    float radar_sweep_angle;

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

/* ---------------------------------------------------------------------
 * Bluetooth SIG Standard UUID & Company ID Resolver
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
    case 0x0087: return "Garmin International";
    case 0x0059: return "Nordic Semiconductor";
    case 0x000D: return "Texas Instruments";
    case 0x013A: return "Fitbit LLC";
    case 0x0046: return "Sony Corp.";
    case 0x0002: return "Intel Corp.";
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
    if (strcasecmp(uuid_str, "0x181a") == 0) return "Environmental Sensing";
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
    if (strcasecmp(uuid_str, "0x2a98") == 0) return "Weight Measurement";
    if (strcasecmp(uuid_str, "0x2a9c") == 0) return "Body Comp Measurement";
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

/* ---------------------------------------------------------------------
 * Scan Worker Task
 * ------------------------------------------------------------------- */

static void ble_scanner_scan_worker(void *arg)
{
    (void)arg;
    for (;;) {
        size_t found = 0U;
        solar_os_ble_keyboard_scan_result_t *scan_buf = malloc(BLE_SCANNER_MAX_DEVICES * sizeof(solar_os_ble_keyboard_scan_result_t));
        if (scan_buf != NULL) {
            const esp_err_t err = solar_os_ble_keyboard_scan(scan_buf, BLE_SCANNER_MAX_DEVICES, &found);

            portENTER_CRITICAL(&ble_scanner_lock);
            if (err == ESP_OK) {
                memcpy(ble_state.staging_results, scan_buf, found * sizeof(ble_state.staging_results[0]));
                ble_state.staging_count = found;
            } else {
                ble_state.staging_count = 0U;
            }
            ble_state.staging_err = err;
            ble_state.staging_ready = true;
            ble_state.task_done = true;
            portEXIT_CRITICAL(&ble_scanner_lock);

            free(scan_buf);
        } else {
            portENTER_CRITICAL(&ble_scanner_lock);
            ble_state.staging_count = 0U;
            ble_state.staging_err = ESP_ERR_NO_MEM;
            ble_state.staging_ready = true;
            ble_state.task_done = true;
            portEXIT_CRITICAL(&ble_scanner_lock);
        }

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
        SOLAR_OS_TASK_ROLE_BACKGROUND
    );

    if (created == pdPASS) {
        ble_state.scanning = true;
        ble_scanner_set_status("Scanning BLE devices...");
    } else {
        ble_state.task = NULL;
        ble_scanner_set_status("Could not start scan task");
    }
    ble_state.render_pending = true;
}

static void ble_scanner_stop_worker(void)
{
    if (ble_state.task == NULL) {
        ble_state.scanning = false;
        return;
    }
    solar_os_task_delete_internal(ble_state.task);
    ble_state.task = NULL;
    ble_state.task_done = true;
    ble_state.scanning = false;
}

/* ---------------------------------------------------------------------
 * Filter Helper
 * ------------------------------------------------------------------- */

static bool ble_device_matches_filter(const ble_device_item_t *dev, ble_filter_mode_t filter)
{
    if (dev == NULL) return false;
    switch (filter) {
    case BLE_FILTER_SAVED:
        return dev->is_bookmarked;
    case BLE_FILTER_NAMED:
        return (dev->alias[0] != '\0') || (dev->name[0] != '\0' && strcmp(dev->name, "(none)") != 0);
    case BLE_FILTER_HID:
        return dev->hid_service || dev->keyboard_like;
    case BLE_FILTER_STRONG:
        return dev->rssi >= -75;
    case BLE_FILTER_ALL:
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

/* ---------------------------------------------------------------------
 * Integrated GATT Explorer Routines
 * ------------------------------------------------------------------- */

static void ble_start_gatt_explorer(const ble_device_item_t *dev)
{
    if (dev == NULL) return;

    /* Crucial: Stop background scan worker so radio is dedicated to GATT connect */
    ble_scanner_stop_worker();

    memcpy(ble_state.connected_bda, dev->bda, 6);
    ble_state.connected_addr_type = dev->addr_type;
    ble_state.connected_rssi = dev->rssi;
    strlcpy(ble_state.connected_name, dev->name[0] ? dev->name : "(Unnamed)", sizeof(ble_state.connected_name));
    strlcpy(ble_state.connected_alias, dev->alias, sizeof(ble_state.connected_alias));

    ble_state.gatt_connecting = true;
    ble_state.gatt_connected = false;
    ble_state.service_count = 0;
    ble_state.selected_service = 0;
    ble_state.char_count = 0;
    ble_state.selected_char = 0;
    ble_state.gatt_focus = GATT_FOCUS_SERVICES;
    ble_state.has_read_value = false;
    ble_state.decoder_mode = GATT_DECODER_AUTO;
    ble_state.view = BLE_VIEW_INTEGRATED_GATT;

    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to %s...", ble_state.connected_alias[0] ? ble_state.connected_alias : ble_state.connected_name);
    ble_scanner_set_status(msg);

    (void)solar_os_ble_gatt_disconnect();

    const esp_err_t err = solar_os_ble_gatt_connect(ble_state.connected_bda, ble_state.connected_addr_type, 6000);
    if (err == ESP_OK) {
        ble_state.gatt_connected = true;
        ble_state.gatt_connecting = false;

        size_t srv_count = 0;
        if (solar_os_ble_gatt_services(ble_state.services, SOLAR_OS_BLE_GATT_MAX_SERVICES, &srv_count) == ESP_OK) {
            ble_state.service_count = srv_count;
            if (srv_count > 0) {
                size_t ch_count = 0;
                solar_os_ble_gatt_characteristics(0, ble_state.characteristics, SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, &ch_count);
                ble_state.char_count = ch_count;
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

static void ble_read_selected_characteristic(void)
{
    if (!ble_state.gatt_connected || ble_state.selected_char >= ble_state.char_count) return;

    const uint16_t handle = ble_state.characteristics[ble_state.selected_char].handle;
    size_t read_len = 0;
    const esp_err_t err = solar_os_ble_gatt_read(handle,
                                                 ble_state.read_value,
                                                 sizeof(ble_state.read_value),
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

static void ble_draw_header(solar_os_gfx_t *gfx, int width)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, BLE_HEADER_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

    const char *view_name = "BLE SCANNER";
    if (ble_state.view == BLE_VIEW_INTEGRATED_GATT) view_name = "DEVICE & GATT EXPLORER";
    else if (ble_state.view == BLE_VIEW_RADAR) view_name = "PROXIMITY RADAR";
    else if (ble_state.view == BLE_VIEW_ADV_INSPECTOR) view_name = "ADV PACKET INSPECTOR";

    char header[96];
    const size_t total_found = ble_filtered_device_count();
    snprintf(header, sizeof(header), "BLE EXPLORER - %s | Dev: %u | %s",
             view_name,
             (unsigned)total_found,
             ble_state.scanning ? "SCANNING..." : "READY");
    solar_os_gfx_text(gfx, 8, 16, header);
}

static void ble_draw_footer(solar_os_gfx_t *gfx, int width, int height)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, height - BLE_FOOTER_H, width, BLE_FOOTER_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    char footer[128];
    if (ble_state.status_until_ms > ble_state.elapsed_ms && ble_state.status_message[0] != '\0') {
        snprintf(footer, sizeof(footer), "%s", ble_state.status_message);
    } else {
        if (ble_state.view == BLE_VIEW_SCANNER) {
            const char *filter_name = ble_state.filter == BLE_FILTER_ALL ? "ALL" :
                                      ble_state.filter == BLE_FILTER_SAVED ? "SAVED" :
                                      ble_state.filter == BLE_FILTER_NAMED ? "NAMED" :
                                      ble_state.filter == BLE_FILTER_HID ? "HID" : "STRONG";
            snprintf(footer, sizeof(footer),
                     "[Enter] Explore | [I] ADV Data | [T] Radar | [A] Alias | [F] Fltr (%s) | [ESC] Exit",
                     filter_name);
        } else if (ble_state.view == BLE_VIEW_ADV_INSPECTOR) {
            const char *mode_str = ble_state.adv_mode == ADV_INSPECT_AUTO ? "AUTO" :
                                  ble_state.adv_mode == ADV_INSPECT_STRUCTURES ? "TLV" :
                                  ble_state.adv_mode == ADV_INSPECT_HEX ? "HEX" : "LUA";
            snprintf(footer, sizeof(footer),
                     "[M] Mode (%s) | [E] Edit Lua | [1-3] Presets | [R] Scan | [ESC] Back",
                     mode_str);
        } else if (ble_state.view == BLE_VIEW_INTEGRATED_GATT) {
            const char *dec_str = ble_state.decoder_mode == GATT_DECODER_AUTO ? "AUTO" :
                                  ble_state.decoder_mode == GATT_DECODER_HEX_ASCII ? "HEX" :
                                  ble_state.decoder_mode == GATT_DECODER_NUMERIC ? "NUM" : "LUA";
            snprintf(footer, sizeof(footer),
                     "[Tab/Arrows] Navigate | [Enter/R] Read | [M] Dec (%s) | [D/ESC] Back",
                     dec_str);
        } else {
            snprintf(footer, sizeof(footer),
                     "[B] Beep (%s) | [R] Scan | [Tab/ESC] Back",
                     ble_state.beep_enabled ? "ON" : "OFF");
        }
    }
    solar_os_gfx_text(gfx, 8, height - 6, footer);
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
 * View 0: BLE Device Scanner List (Alias + Original Name)
 * ------------------------------------------------------------------- */

static void ble_draw_scanner_list(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = BLE_HEADER_H + 4;
    const int bottom = height - BLE_FOOTER_H - 4;
    const int row_h = 32;
    const int visible_rows = (bottom - top) / row_h;

    const size_t total_visible = ble_filtered_device_count();
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

    if (ble_state.selected_device >= total_visible) {
        ble_state.selected_device = total_visible - 1U;
    }

    size_t scroll_offset = 0;
    if (ble_state.selected_device >= (size_t)visible_rows) {
        scroll_offset = ble_state.selected_device - (size_t)visible_rows + 1U;
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
        char tags[32] = "";
        if (dev->adv_data_len > 0) {
            char adv_tag[16];
            snprintf(adv_tag, sizeof(adv_tag), "[ADV %uB] ", (unsigned)dev->adv_data_len);
            strlcat(tags, adv_tag, sizeof(tags));
        }
        if (dev->keyboard_like) strlcat(tags, "[KBD] ", sizeof(tags));
        else if (dev->hid_service) strlcat(tags, "[HID] ", sizeof(tags));
        if (tags[0] != '\0') {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, width - 110, y + 13, tags);
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

    /* Modal dialog for editing alias */
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
        solar_os_gfx_text(gfx, mx + 10, my + 20, "Set Custom Device Alias:");

        solar_os_gfx_rect(gfx, mx + 10, my + 30, mw - 20, 24);
        char disp_input[40];
        snprintf(disp_input, sizeof(disp_input), "%s_", ble_state.alias_input);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, mx + 15, my + 46, disp_input);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, mx + 10, my + 68, "[Enter] Save  |  [ESC] Cancel / Clear");
    }
}

/* ---------------------------------------------------------------------
 * View 1: Integrated Single-Screen Device Details & GATT Explorer
 * ------------------------------------------------------------------- */

static void ble_render_gatt_decoded_value(solar_os_gfx_t *gfx, int x, int y)
{
    if (!ble_state.has_read_value || ble_state.read_value_len == 0) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, x, y, "Press [Enter] or [R] to read characteristic live value.");
        return;
    }

    const char *uuid_str = ble_state.characteristics[ble_state.selected_char].uuid;
    const uint8_t *val = ble_state.read_value;
    const size_t len = ble_state.read_value_len;

    switch (ble_state.decoder_mode) {
    case GATT_DECODER_AUTO: {
        char decoded[128] = "";
        if (strcasecmp(uuid_str, "0x2a19") == 0 && len >= 1) {
            snprintf(decoded, sizeof(decoded), "BATTERY LEVEL: %u%%", val[0]);
        } else if (strcasecmp(uuid_str, "0x2a6e") == 0 && len >= 2) {
            int16_t temp_raw = (int16_t)((uint16_t)val[0] | ((uint16_t)val[1] << 8));
            snprintf(decoded, sizeof(decoded), "TEMPERATURE: %.2f °C", (float)temp_raw / 100.0f);
        } else if (strcasecmp(uuid_str, "0x2a6f") == 0 && len >= 2) {
            uint16_t hum_raw = (uint16_t)val[0] | ((uint16_t)val[1] << 8);
            snprintf(decoded, sizeof(decoded), "HUMIDITY: %.2f %%", (float)hum_raw / 100.0f);
        } else if (strcasecmp(uuid_str, "0x2a37") == 0 && len >= 2) {
            snprintf(decoded, sizeof(decoded), "HEART RATE: %u BPM", val[1]);
        } else if (strcasecmp(uuid_str, "0x2a98") == 0 && len >= 3) {
            uint16_t raw_w = (uint16_t)val[1] | ((uint16_t)val[2] << 8);
            snprintf(decoded, sizeof(decoded), "WEIGHT (BASKUL): %.2f kg", (float)raw_w * 0.005f);
        } else if (strcasecmp(uuid_str, "0x2a00") == 0 || strcasecmp(uuid_str, "0x2a24") == 0 ||
                   strcasecmp(uuid_str, "0x2a25") == 0 || strcasecmp(uuid_str, "0x2a26") == 0 ||
                   strcasecmp(uuid_str, "0x2a28") == 0 || strcasecmp(uuid_str, "0x2a29") == 0) {
            char str_buf[64] = "";
            size_t slen = len < sizeof(str_buf) - 1 ? len : sizeof(str_buf) - 1;
            memcpy(str_buf, val, slen);
            str_buf[slen] = '\0';
            snprintf(decoded, sizeof(decoded), "STRING: \"%s\"", str_buf);
        } else {
            char hex_buf[48] = "";
            char asc_buf[24] = "";
            for (size_t i = 0; i < len && i < 8; i++) {
                char h[8];
                snprintf(h, sizeof(h), "%02X ", val[i]);
                strlcat(hex_buf, h, sizeof(hex_buf));
                char c = (char)val[i];
                char a[2] = { isprint((unsigned char)c) ? c : '.', '\0' };
                strlcat(asc_buf, a, sizeof(asc_buf));
            }
            snprintf(decoded, sizeof(decoded), "HEX: %s | ASCII: '%s'", hex_buf, asc_buf);
        }
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
        solar_os_gfx_text(gfx, x, y + 4, decoded);
        break;
    }

    case GATT_DECODER_HEX_ASCII: {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        char hex_dump[128] = "HEX: ";
        char ascii_dump[64] = "ASC: ";
        for (size_t i = 0; i < len && i < 16; i++) {
            char byte_hex[8];
            snprintf(byte_hex, sizeof(byte_hex), "%02X ", val[i]);
            strlcat(hex_dump, byte_hex, sizeof(hex_dump));

            const char c = (char)val[i];
            char char_str[2] = { isprint((unsigned char)c) ? c : '.', '\0' };
            strlcat(ascii_dump, char_str, sizeof(ascii_dump));
        }
        solar_os_gfx_text(gfx, x, y, hex_dump);
        solar_os_gfx_text(gfx, x, y + 16, ascii_dump);
        break;
    }

    case GATT_DECODER_NUMERIC: {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        char num1[96] = "";
        char num2[96] = "";
        if (len >= 1) {
            snprintf(num1, sizeof(num1), "u8: %u  |  i8: %d", val[0], (int8_t)val[0]);
        }
        if (len >= 2) {
            uint16_t u16_le = (uint16_t)val[0] | ((uint16_t)val[1] << 8);
            uint16_t u16_be = ((uint16_t)val[0] << 8) | (uint16_t)val[1];
            char extra[64];
            snprintf(extra, sizeof(extra), "  |  u16LE: %u  |  u16BE: %u", u16_le, u16_be);
            strlcat(num1, extra, sizeof(num1));
        }
        if (len >= 4) {
            uint32_t u32_le = (uint32_t)val[0] | ((uint32_t)val[1] << 8) | ((uint32_t)val[2] << 16) | ((uint32_t)val[3] << 24);
            float f32;
            memcpy(&f32, val, 4);
            snprintf(num2, sizeof(num2), "u32LE: %" PRIu32 "  |  float32: %.4f", u32_le, f32);
        }
        solar_os_gfx_text(gfx, x, y, num1);
        if (num2[0] != '\0') {
            solar_os_gfx_text(gfx, x, y + 16, num2);
        }
        break;
    }

    case GATT_DECODER_LUA: {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        char lua_code[256];
        snprintf(lua_code, sizeof(lua_code),
                 "bytes = {%u,%u,%u,%u,%u,%u,%u,%u}; "
                 "return 'Lua: ' .. tostring((bytes[1] or 0) + (bytes[2] or 0)*256)",
                 len > 0 ? val[0] : 0, len > 1 ? val[1] : 0, len > 2 ? val[2] : 0, len > 3 ? val[3] : 0,
                 len > 4 ? val[4] : 0, len > 5 ? val[5] : 0, len > 6 ? val[6] : 0, len > 7 ? val[7] : 0);
        char out_buf[96] = "Lua: (engine ready)";
        solar_os_script_run_request_t req = {
            .input_type = SOLAR_OS_SCRIPT_INPUT_SOURCE,
            .input = lua_code,
            .input_len = strlen(lua_code),
            .source_name = "ble_eval",
            .timeout_ms = 500,
            .output = out_buf,
            .output_size = sizeof(out_buf),
        };
        solar_os_script_run_result_t res = {0};
        if (solar_os_lua_run(&req, &res) == ESP_OK && res.success) {
            solar_os_gfx_text(gfx, x, y, out_buf[0] ? out_buf : "Lua parsed successfully");
        } else {
            char fallback[64];
            snprintf(fallback, sizeof(fallback), "Lua bytes: {%u, %u, %u, %u}",
                     len > 0 ? val[0] : 0, len > 1 ? val[1] : 0, len > 2 ? val[2] : 0, len > 3 ? val[3] : 0);
            solar_os_gfx_text(gfx, x, y, fallback);
        }
        break;
    }

    default:
        break;
    }
}

static void ble_draw_integrated_gatt(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = BLE_HEADER_H + 2;
    const int bottom = height - BLE_FOOTER_H - 2;

    /* 1. Top Device Details Banner */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

    char dev_title[96];
    if (ble_state.connected_alias[0] != '\0' && strcmp(ble_state.connected_alias, ble_state.connected_name) != 0) {
        snprintf(dev_title, sizeof(dev_title), "* [%s] (%s)", ble_state.connected_alias, ble_state.connected_name);
    } else {
        snprintf(dev_title, sizeof(dev_title), "%s", ble_state.connected_alias[0] ? ble_state.connected_alias : ble_state.connected_name);
    }
    solar_os_gfx_text(gfx, 6, top + 11, dev_title);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char dev_details[96];
    snprintf(dev_details, sizeof(dev_details), "MAC: %02X:%02X:%02X:%02X:%02X:%02X | %ddBm | %s",
             ble_state.connected_bda[0], ble_state.connected_bda[1], ble_state.connected_bda[2],
             ble_state.connected_bda[3], ble_state.connected_bda[4], ble_state.connected_bda[5],
             (int)ble_state.connected_rssi,
             ble_state.gatt_connected ? "CONNECTED" : (ble_state.gatt_connecting ? "CONNECTING..." : "NOT CONNECTED"));
    solar_os_gfx_text(gfx, 6, top + 24, dev_details);

    solar_os_gfx_line(gfx, 4, top + 28, width - 4, top + 28);

    /* 2. Middle Split Columns: Services (Left) & Characteristics (Right) */
    const int mid_top = top + 32;
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
        char c_label[48];
        snprintf(c_label, sizeof(c_label), "%s", name);
        solar_os_gfx_text(gfx, split_x + 8, y + 15, c_label);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        char c_props[32];
        snprintf(c_props, sizeof(c_props), "0x%04X (%s)", (unsigned)ch->handle, ch->uuid);
        solar_os_gfx_text(gfx, width - 110, y + 15, c_props);

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    }

    /* 3. Bottom Box: Live Decoded Characteristic Inspector */
    const int box_top = mid_top + mid_h + 4;
    const int box_h = bottom - box_top;
    if (box_h > 20) {
        solar_os_gfx_rect(gfx, 4, box_top, width - 8, box_h);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

        const char *mode_label = ble_state.decoder_mode == GATT_DECODER_AUTO ? "[MODE: AUTO SENSOR]" :
                                 ble_state.decoder_mode == GATT_DECODER_HEX_ASCII ? "[MODE: HEX & ASCII]" :
                                 ble_state.decoder_mode == GATT_DECODER_NUMERIC ? "[MODE: NUMERIC INT/FLOAT]" :
                                 "[MODE: LUA SCRIPT EVAL]";
        char title_buf[64];
        snprintf(title_buf, sizeof(title_buf), "Value Inspector %s ([M] Change):", mode_label);
        solar_os_gfx_text(gfx, 8, box_top + 14, title_buf);

        ble_render_gatt_decoded_value(gfx, 8, box_top + 32);
    }
}

/* ---------------------------------------------------------------------
 * View 3: BLE Advertising Packet Inspector & Robust Lua Evaluator
 * ------------------------------------------------------------------- */

static void ble_draw_adv_inspector(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = BLE_HEADER_H + 4;
    const int bottom = height - BLE_FOOTER_H - 4;

    const ble_device_item_t *dev = ble_get_filtered_device(ble_state.selected_device);
    if (dev == NULL) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 20, top + 30, "No device selected.");
        return;
    }

    /* Top Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char title[80];
    if (dev->alias[0] != '\0' && dev->name[0] != '\0' && strcmp(dev->alias, dev->name) != 0) {
        snprintf(title, sizeof(title), "* [%s] (%s)", dev->alias, dev->name);
    } else {
        snprintf(title, sizeof(title), "%s", dev->alias[0] ? dev->alias : (dev->name[0] ? dev->name : "(Unnamed)"));
    }

    char dev_hdr[110];
    snprintf(dev_hdr, sizeof(dev_hdr), "ADV: %s [%02X:%02X:%02X:%02X:%02X:%02X] %ddBm (Len: %u)",
             title,
             dev->bda[0], dev->bda[1], dev->bda[2], dev->bda[3], dev->bda[4], dev->bda[5],
             (int)dev->rssi, (unsigned)dev->adv_data_len);
    solar_os_gfx_text(gfx, 8, top + 14, dev_hdr);
    solar_os_gfx_line(gfx, 4, top + 20, width - 4, top + 20);

    const int content_y = top + 26;
    const uint8_t *adv = dev->adv_data;
    const size_t len = dev->adv_data_len;

    switch (ble_state.adv_mode) {
    case ADV_INSPECT_AUTO: {
        int cur_y = content_y + 12;

        /* Look for Weight Scale (Baskul) data */
        bool is_scale = false;
        char scale_reading[96] = "";

        for (size_t i = 0; i + 1 < len;) {
            uint8_t ad_len = adv[i];
            if (ad_len == 0 || i + 1 + ad_len > len) break;
            uint8_t ad_type = adv[i + 1];
            const uint8_t *ad_payload = &adv[i + 2];
            uint8_t payload_len = ad_len - 1;

            if (ad_type == 0x16 && payload_len >= 4) {
                uint16_t srv_uuid = (uint16_t)ad_payload[0] | ((uint16_t)ad_payload[1] << 8);
                if (srv_uuid == 0x181D || srv_uuid == 0x181B) {
                    is_scale = true;
                    uint8_t flags = ad_payload[2];
                    const char *unit = (flags & 0x01) ? "lbs" : "kg";
                    uint16_t raw_w = (uint16_t)ad_payload[3] | ((uint16_t)ad_payload[4] << 8);
                    float weight_kg = (flags & 0x01) ? ((float)raw_w * 0.01f) : ((float)raw_w * 0.005f);
                    snprintf(scale_reading, sizeof(scale_reading), "WEIGHT SCALE (BASKUL): %.2f %s (Flags: 0x%02X)",
                             weight_kg, unit, flags);
                    break;
                }
            }

            if (ad_type == 0xFF && payload_len >= 12) {
                uint16_t comp_id = (uint16_t)ad_payload[0] | ((uint16_t)ad_payload[1] << 8);
                if (comp_id == 0x038F || comp_id == 0x0157) {
                    is_scale = true;
                    uint8_t flags = ad_payload[2];
                    const char *unit = (flags & 0x01) ? "lbs" : ((flags & 0x02) ? "jin" : "kg");
                    bool stabilized = (flags & 0x20) != 0;
                    uint16_t raw_w = (uint16_t)ad_payload[11] | ((uint16_t)ad_payload[12] << 8);
                    float w = (flags & 0x01) ? ((float)raw_w / 100.0f) : ((float)raw_w / 200.0f);
                    snprintf(scale_reading, sizeof(scale_reading), "XIAOMI SCALE (BASKUL): %.2f %s [%s]",
                             w, unit, stabilized ? "STABILIZED" : "MEASURING...");
                    break;
                }
            }
            i += (1 + ad_len);
        }

        if (is_scale) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 6, cur_y - 12, width - 12, 28);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
            solar_os_gfx_text(gfx, 12, cur_y + 6, scale_reading);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            cur_y += 32;
        }

        /* Decode Manufacturer Data / Beacons */
        for (size_t i = 0; i + 1 < len;) {
            uint8_t ad_len = adv[i];
            if (ad_len == 0 || i + 1 + ad_len > len) break;
            uint8_t ad_type = adv[i + 1];
            const uint8_t *ad_payload = &adv[i + 2];
            uint8_t payload_len = ad_len - 1;

            if (ad_type == 0xFF && payload_len >= 2) {
                uint16_t comp_id = (uint16_t)ad_payload[0] | ((uint16_t)ad_payload[1] << 8);
                const char *comp_name = ble_resolve_company_name(comp_id);
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
                char comp_str[96];
                snprintf(comp_str, sizeof(comp_str), "Manufacturer: 0x%04X (%s)", comp_id, comp_name);
                solar_os_gfx_text(gfx, 10, cur_y, comp_str);
                cur_y += 18;

                if (comp_id == 0x004C && payload_len >= 23 && ad_payload[2] == 0x02 && ad_payload[3] == 0x15) {
                    uint16_t major = ((uint16_t)ad_payload[20] << 8) | (uint16_t)ad_payload[21];
                    uint16_t minor = ((uint16_t)ad_payload[22] << 8) | (uint16_t)ad_payload[23];
                    int8_t tx_pow = (int8_t)ad_payload[24];

                    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
                    solar_os_gfx_text(gfx, 10, cur_y, ">> iBeacon Frame Detected <<");
                    cur_y += 16;
                    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                    char ib_str[96];
                    snprintf(ib_str, sizeof(ib_str), "Major: %u | Minor: %u | TxPower@1m: %ddBm", major, minor, (int)tx_pow);
                    solar_os_gfx_text(gfx, 10, cur_y, ib_str);
                    cur_y += 16;
                } else {
                    char m_hex[64] = "Payload: ";
                    for (size_t m = 2; m < payload_len && m < 12; m++) {
                        char bh[8];
                        snprintf(bh, sizeof(bh), "%02X ", ad_payload[m]);
                        strlcat(m_hex, bh, sizeof(m_hex));
                    }
                    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                    solar_os_gfx_text(gfx, 10, cur_y, m_hex);
                    cur_y += 18;
                }
            } else if (ad_type == 0x16 && payload_len >= 2) {
                uint16_t srv_uuid = (uint16_t)ad_payload[0] | ((uint16_t)ad_payload[1] << 8);
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
                char srv_str[96];
                snprintf(srv_str, sizeof(srv_str), "Service Data: 0x%04X (%s)", srv_uuid,
                         (srv_uuid == 0xFEAA) ? "Google Eddystone Beacon" :
                         (srv_uuid == 0xFCD2) ? "BTHome V2" : "BLE Service");
                solar_os_gfx_text(gfx, 10, cur_y, srv_str);
                cur_y += 18;
            }
            i += (1 + ad_len);
        }

        /* Raw Hex Packet Strip */
        const int hex_box_y = bottom - 46;
        solar_os_gfx_rect(gfx, 6, hex_box_y, width - 12, 42);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 10, hex_box_y + 12, "Raw Advertisement Bytes (HEX):");

        char hex_line[128] = "";
        for (size_t h = 0; h < len && h < 18; h++) {
            char bh[8];
            snprintf(bh, sizeof(bh), "%02X ", adv[h]);
            strlcat(hex_line, bh, sizeof(hex_line));
        }
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 10, hex_box_y + 26, hex_line[0] ? hex_line : "(No bytes captured yet)");
        break;
    }

    case ADV_INSPECT_STRUCTURES: {
        int cur_y = content_y + 12;
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 10, cur_y, "Advertised TLV Elements:");
        cur_y += 18;

        for (size_t i = 0; i + 1 < len;) {
            uint8_t ad_len = adv[i];
            if (ad_len == 0 || i + 1 + ad_len > len) break;
            uint8_t ad_type = adv[i + 1];
            const uint8_t *ad_payload = &adv[i + 2];
            uint8_t payload_len = ad_len - 1;

            const char *type_name = (ad_type == 0x01) ? "Flags" :
                                    (ad_type == 0x02 || ad_type == 0x03) ? "16-bit Service UUIDs" :
                                    (ad_type == 0x08 || ad_type == 0x09) ? "Local Name" :
                                    (ad_type == 0x0A) ? "Tx Power" :
                                    (ad_type == 0x16) ? "Service Data" :
                                    (ad_type == 0xFF) ? "Manufacturer Specific" : "Custom AD";

            char tlv_buf[128];
            char hex_snippet[48] = "";
            for (size_t k = 0; k < payload_len && k < 6; k++) {
                char h[8];
                snprintf(h, sizeof(h), "%02X ", ad_payload[k]);
                strlcat(hex_snippet, h, sizeof(hex_snippet));
            }
            snprintf(tlv_buf, sizeof(tlv_buf), "Type 0x%02X (%s) [Len %u]: %s",
                     ad_type, type_name, payload_len, hex_snippet);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 10, cur_y, tlv_buf);
            cur_y += 16;
            if (cur_y > bottom - 10) break;

            i += (1 + ad_len);
        }
        break;
    }

    case ADV_INSPECT_HEX: {
        int cur_y = content_y + 12;
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 10, cur_y, "Full 62-byte Advertisement Payload Dump:");
        cur_y += 20;

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO);
        for (size_t row = 0; row < len && row < 48; row += 16) {
            char row_hex[64] = "";
            char row_asc[32] = "";
            for (size_t col = 0; col < 16; col++) {
                if (row + col < len) {
                    char bh[8];
                    snprintf(bh, sizeof(bh), "%02X ", adv[row + col]);
                    strlcat(row_hex, bh, sizeof(row_hex));
                    char c = (char)adv[row + col];
                    char a[2] = { isprint((unsigned char)c) ? c : '.', '\0' };
                    strlcat(row_asc, a, sizeof(row_asc));
                } else {
                    strlcat(row_hex, "   ", sizeof(row_hex));
                }
            }
            char full_line[128];
            snprintf(full_line, sizeof(full_line), "%02X:  %s | %s", (unsigned)row, row_hex, row_asc);
            solar_os_gfx_text(gfx, 10, cur_y, full_line);
            cur_y += 18;
        }
        break;
    }

    case ADV_INSPECT_LUA: {
        int cur_y = content_y + 12;
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 10, cur_y, "Lua Script Custom Packet Evaluator ([E] to Edit | [1-3] Presets):");
        cur_y += 20;

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        char expr_line[180];
        const char *expr = ble_state.custom_lua_expr[0] ? ble_state.custom_lua_expr :
            "return string.format('Scale: %.2f kg (B12=%d)', ((adv[12] or 0) + (adv[13] or 0)*256)/200.0, adv[12] or 0)";
        snprintf(expr_line, sizeof(expr_line), "Script: %s", expr);
        solar_os_gfx_text(gfx, 10, cur_y, expr_line);
        cur_y += 22;

        char lua_full_code[512] = "adv = {";
        for (size_t b = 0; b < len && b < 32; b++) {
            char bh[16];
            snprintf(bh, sizeof(bh), "%u,", adv[b]);
            strlcat(lua_full_code, bh, sizeof(lua_full_code));
        }
        strlcat(lua_full_code, "}; bda = '", sizeof(lua_full_code));
        char mac_str[32];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 dev->bda[0], dev->bda[1], dev->bda[2], dev->bda[3], dev->bda[4], dev->bda[5]);
        strlcat(lua_full_code, mac_str, sizeof(lua_full_code));
        strlcat(lua_full_code, "'; rssi = ", sizeof(lua_full_code));
        char rssi_str[16];
        snprintf(rssi_str, sizeof(rssi_str), "%d; ", (int)dev->rssi);
        strlcat(lua_full_code, rssi_str, sizeof(lua_full_code));
        strlcat(lua_full_code, expr, sizeof(lua_full_code));

        char lua_out[128] = "Lua: (Ready)";
        solar_os_script_run_request_t req = {
            .input_type = SOLAR_OS_SCRIPT_INPUT_SOURCE,
            .input = lua_full_code,
            .input_len = strlen(lua_full_code),
            .source_name = "adv_eval",
            .timeout_ms = 500,
            .output = lua_out,
            .output_size = sizeof(lua_out),
        };
        solar_os_script_run_result_t res = {0};

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, 6, cur_y, width - 12, 48);

        if (solar_os_lua_run(&req, &res) == ESP_OK && res.success) {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
            solar_os_gfx_text(gfx, 12, cur_y + 20, lua_out[0] ? lua_out : "Result: (nil)");
        } else {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 12, cur_y + 18, "Lua Eval Output: Error in script");
            if (res.error[0] != '\0') {
                solar_os_gfx_text(gfx, 12, cur_y + 34, res.error);
            }
        }
        break;
    }

    default:
        break;
    }

    /* Modal dialog for editing custom Lua expression */
    if (ble_state.editing_lua) {
        const int mw = 360;
        const int mh = 86;
        const int mx = (width - mw) / 2;
        const int my = (height - mh) / 2;

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, mx, my, mw, mh);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, mx, my, mw, mh);
        solar_os_gfx_rect(gfx, mx + 2, my + 2, mw - 4, mh - 4);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, mx + 10, my + 18, "Edit Lua Code (e.g. return adv[12] .. ' kg'):");

        solar_os_gfx_rect(gfx, mx + 10, my + 28, mw - 20, 26);
        char disp_lua[180];
        snprintf(disp_lua, sizeof(disp_lua), "%s_", ble_state.lua_input);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, mx + 14, my + 45, disp_lua);

        solar_os_gfx_text(gfx, mx + 10, my + 72, "[Enter] Save  |  [ESC] Cancel");
    }
}

/* ---------------------------------------------------------------------
 * View 2: Proximity Radar / Signal Tracker
 * ------------------------------------------------------------------- */

static void ble_draw_radar(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = BLE_HEADER_H + 4;
    const int bottom = height - BLE_FOOTER_H - 4;
    const int cx = 130;
    const int cy = top + (bottom - top) / 2;
    const int max_radius = 80;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_circle(gfx, cx, cy, max_radius);
    solar_os_gfx_circle(gfx, cx, cy, max_radius * 2 / 3);
    solar_os_gfx_circle(gfx, cx, cy, max_radius / 3);
    solar_os_gfx_line(gfx, cx - max_radius, cy, cx + max_radius, cy);
    solar_os_gfx_line(gfx, cx, cy - max_radius, cx, cy + max_radius);

    const float rad = ble_state.radar_sweep_angle;
    const int sx = cx + (int)(cosf(rad) * (float)max_radius);
    const int sy = cy + (int)(sinf(rad) * (float)max_radius);
    solar_os_gfx_line(gfx, cx, cy, sx, sy);

    int8_t rssi = ble_state.tracked_rssi;
    if (rssi > -30) rssi = -30;
    if (rssi < -95) rssi = -95;
    const float norm_dist = (float)(-rssi - 30) / 65.0f;
    const int blip_r = (int)(norm_dist * (float)(max_radius - 6));
    const int bx = cx + (int)(cosf(rad - 0.5f) * (float)blip_r);
    const int by = cy + (int)(sinf(rad - 0.5f) * (float)blip_r);
    solar_os_gfx_fill_circle(gfx, bx, by, 5);

    const int info_x = 240;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_text(gfx, info_x, top + 24, "BLE RADAR TRACKER");

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char name_buf[48];
    snprintf(name_buf, sizeof(name_buf), "%s", ble_state.tracked_name);
    solar_os_gfx_text(gfx, info_x, top + 46, name_buf);

    char mac_buf[48];
    snprintf(mac_buf, sizeof(mac_buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             ble_state.tracked_bda[0], ble_state.tracked_bda[1], ble_state.tracked_bda[2],
             ble_state.tracked_bda[3], ble_state.tracked_bda[4], ble_state.tracked_bda[5]);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, info_x, top + 64, mac_buf);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    char rssi_buf[32];
    snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm", (int)ble_state.tracked_rssi);
    solar_os_gfx_text(gfx, info_x, top + 98, rssi_buf);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    const char *prox = (ble_state.tracked_rssi >= -60) ? "PROXIMITY: IMMEDIATE (<0.5m)" :
                       (ble_state.tracked_rssi >= -75) ? "PROXIMITY: NEAR (1-3m)" :
                       "PROXIMITY: FAR (>3m)";
    solar_os_gfx_text(gfx, info_x, top + 120, prox);

    const int spark_y = top + 144;
    const int spark_w = width - info_x - 12;
    const int spark_h = 36;
    solar_os_gfx_rect(gfx, info_x, spark_y, spark_w, spark_h);
    if (ble_state.rssi_history_count > 1) {
        int prev_px = info_x;
        int prev_py = spark_y + spark_h / 2;
        for (size_t i = 0; i < ble_state.rssi_history_count; i++) {
            const int px = info_x + (int)((float)i / (float)(ble_state.rssi_history_count - 1) * (float)spark_w);
            int8_t hist_rssi = ble_state.rssi_history[i];
            if (hist_rssi > -30) hist_rssi = -30;
            if (hist_rssi < -95) hist_rssi = -95;
            const float h_norm = (float)(-hist_rssi - 30) / 65.0f;
            const int py = spark_y + (int)(h_norm * (float)spark_h);
            if (i > 0) {
                solar_os_gfx_line(gfx, prev_px, prev_py, px, py);
            }
            prev_px = px;
            prev_py = py;
        }
    }
}

/* ---------------------------------------------------------------------
 * Main App Render
 * ------------------------------------------------------------------- */

static void ble_scanner_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    ble_draw_header(gfx, width);

    switch (ble_state.view) {
    case BLE_VIEW_INTEGRATED_GATT:
        ble_draw_integrated_gatt(gfx, width, height);
        break;
    case BLE_VIEW_RADAR:
        ble_draw_radar(gfx, width, height);
        break;
    case BLE_VIEW_ADV_INSPECTOR:
        ble_draw_adv_inspector(gfx, width, height);
        break;
    case BLE_VIEW_SCANNER:
    default:
        ble_draw_scanner_list(gfx, width, height);
        break;
    }

    ble_draw_footer(gfx, width, height);
    solar_os_gfx_present(gfx);
    ble_state.render_pending = false;
}

/* ---------------------------------------------------------------------
 * Event & Key Handling
 * ------------------------------------------------------------------- */

static void ble_scanner_handle_char(solar_os_context_t *ctx, char ch)
{
    const unsigned char uch = (unsigned char)ch;

    /* Handle typing when in Alias Edit Modal */
    if (ble_state.editing_alias) {
        if (uch == SOLAR_OS_KEY_ESCAPE) {
            ble_state.editing_alias = false;
            ble_state.render_pending = true;
            return;
        }
        if (ch == '\n' || ch == '\r') {
            ble_device_item_t *dev = ble_get_filtered_device(ble_state.selected_device);
            if (dev != NULL) {
                strlcpy(dev->alias, ble_state.alias_input, sizeof(dev->alias));
                dev->is_bookmarked = (dev->alias[0] != '\0');

                ble_bookmark_t *bm = ble_find_bookmark(dev->bda);
                if (dev->is_bookmarked) {
                    if (bm == NULL && ble_state.bookmark_count < BLE_SCANNER_MAX_BOOKMARKS) {
                        bm = &ble_state.bookmarks[ble_state.bookmark_count++];
                        memcpy(bm->bda, dev->bda, 6);
                        bm->alert_enabled = true;
                        bm->alert_threshold_rssi = -75;
                    }
                    if (bm != NULL) {
                        strlcpy(bm->alias, dev->alias, sizeof(bm->alias));
                    }
                } else if (bm != NULL) {
                    size_t idx = (size_t)(bm - ble_state.bookmarks);
                    for (size_t b = idx; b + 1 < ble_state.bookmark_count; b++) {
                        ble_state.bookmarks[b] = ble_state.bookmarks[b + 1];
                    }
                    if (ble_state.bookmark_count > 0) ble_state.bookmark_count--;
                }
                ble_scanner_save_bookmarks();
                char msg[64];
                snprintf(msg, sizeof(msg), "Alias saved: '%s'", dev->alias);
                ble_scanner_set_status(msg);
            }
            ble_state.editing_alias = false;
            ble_state.render_pending = true;
            return;
        }
        if (uch == '\b' || uch == 127 || uch == 8) {
            if (ble_state.alias_cursor > 0) {
                ble_state.alias_cursor--;
                ble_state.alias_input[ble_state.alias_cursor] = '\0';
                ble_state.render_pending = true;
            }
            return;
        }
        if (isprint((unsigned char)ch) && ble_state.alias_cursor + 1 < sizeof(ble_state.alias_input)) {
            ble_state.alias_input[ble_state.alias_cursor++] = ch;
            ble_state.alias_input[ble_state.alias_cursor] = '\0';
            ble_state.render_pending = true;
            return;
        }
        return;
    }

    /* Handle typing when in Custom Lua Edit Modal */
    if (ble_state.editing_lua) {
        if (uch == SOLAR_OS_KEY_ESCAPE) {
            ble_state.editing_lua = false;
            ble_state.render_pending = true;
            return;
        }
        if (ch == '\n' || ch == '\r') {
            strlcpy(ble_state.custom_lua_expr, ble_state.lua_input, sizeof(ble_state.custom_lua_expr));
            ble_state.editing_lua = false;
            ble_scanner_set_status("Custom Lua expression saved");
            ble_state.render_pending = true;
            return;
        }
        if (uch == '\b' || uch == 127 || uch == 8) {
            if (ble_state.lua_cursor > 0) {
                ble_state.lua_cursor--;
                ble_state.lua_input[ble_state.lua_cursor] = '\0';
                ble_state.render_pending = true;
            }
            return;
        }
        if (isprint((unsigned char)ch) && ble_state.lua_cursor + 1 < sizeof(ble_state.lua_input)) {
            ble_state.lua_input[ble_state.lua_cursor++] = ch;
            ble_state.lua_input[ble_state.lua_cursor] = '\0';
            ble_state.render_pending = true;
            return;
        }
        return;
    }

    if (uch == SOLAR_OS_KEY_ESCAPE) {
        if (ble_state.view == BLE_VIEW_INTEGRATED_GATT || ble_state.view == BLE_VIEW_RADAR || ble_state.view == BLE_VIEW_ADV_INSPECTOR) {
            (void)solar_os_ble_gatt_disconnect();
            ble_state.view = BLE_VIEW_SCANNER;
            ble_state.render_pending = true;
            return;
        }
        ble_scanner_stop_worker();
        (void)solar_os_ble_gatt_disconnect();
        solar_os_context_request_exit(ctx);
        return;
    }

    if (ble_state.view == BLE_VIEW_SCANNER) {
        const size_t total_visible = ble_filtered_device_count();

        if (uch == SOLAR_OS_KEY_UP) {
            if (ble_state.selected_device > 0) {
                ble_state.selected_device--;
                ble_state.render_pending = true;
            }
            return;
        }
        if (uch == SOLAR_OS_KEY_DOWN) {
            if (total_visible > 0 && ble_state.selected_device + 1 < total_visible) {
                ble_state.selected_device++;
                ble_state.render_pending = true;
            }
            return;
        }
        if (ch == ' ' || ch == 'r' || ch == 'R') {
            ble_scanner_start_scan();
            return;
        }
        if (ch == 'f' || ch == 'F') {
            ble_state.filter = (ble_filter_mode_t)((ble_state.filter + 1) % BLE_FILTER_COUNT);
            ble_state.selected_device = 0;
            ble_state.render_pending = true;
            return;
        }
        if (ch == 'a' || ch == 'A' || ch == 's' || ch == 'S') {
            const ble_device_item_t *dev = ble_get_filtered_device(ble_state.selected_device);
            if (dev != NULL) {
                strlcpy(ble_state.alias_input, dev->alias, sizeof(ble_state.alias_input));
                ble_state.alias_cursor = strlen(ble_state.alias_input);
                ble_state.editing_alias = true;
                ble_state.render_pending = true;
            }
            return;
        }
        if (ch == 'i' || ch == 'I') {
            if (total_visible > 0) {
                ble_state.view = BLE_VIEW_ADV_INSPECTOR;
                ble_state.adv_mode = ADV_INSPECT_AUTO;
                ble_state.render_pending = true;
            }
            return;
        }
        if (ch == 't' || ch == 'T') {
            const ble_device_item_t *dev = ble_get_filtered_device(ble_state.selected_device);
            if (dev != NULL) {
                memcpy(ble_state.tracked_bda, dev->bda, 6);
                const char *dev_lbl = dev->alias[0] ? dev->alias : (dev->name[0] ? dev->name : "(Unnamed)");
                strlcpy(ble_state.tracked_name, dev_lbl, sizeof(ble_state.tracked_name));
                ble_state.tracked_rssi = dev->rssi;
                ble_state.rssi_history_count = 0;
                ble_state.view = BLE_VIEW_RADAR;
                ble_state.render_pending = true;
            }
            return;
        }
        if (ch == '\n' || ch == '\r') {
            const ble_device_item_t *dev = ble_get_filtered_device(ble_state.selected_device);
            if (dev != NULL) {
                ble_start_gatt_explorer(dev);
            }
            return;
        }
        return;
    }

    if (ble_state.view == BLE_VIEW_ADV_INSPECTOR) {
        if (ch == 'm' || ch == 'M') {
            ble_state.adv_mode = (adv_inspect_mode_t)((ble_state.adv_mode + 1) % ADV_INSPECT_COUNT);
            ble_state.render_pending = true;
            return;
        }
        if (ch == 'e' || ch == 'E') {
            strlcpy(ble_state.lua_input, ble_state.custom_lua_expr[0] ? ble_state.custom_lua_expr :
                    "return string.format('Scale: %.2f kg', ((adv[12] or 0) + (adv[13] or 0)*256)/200.0)",
                    sizeof(ble_state.lua_input));
            ble_state.lua_cursor = strlen(ble_state.lua_input);
            ble_state.editing_lua = true;
            ble_state.adv_mode = ADV_INSPECT_LUA;
            ble_state.render_pending = true;
            return;
        }
        /* Presets */
        if (ch == '1') {
            strlcpy(ble_state.custom_lua_expr,
                    "return string.format('Xiaomi Scale 2: %.2f kg', ((adv[12] or 0) + (adv[13] or 0)*256)/200.0)",
                    sizeof(ble_state.custom_lua_expr));
            ble_state.adv_mode = ADV_INSPECT_LUA;
            ble_scanner_set_status("Loaded Xiaomi Scale 2 preset");
            ble_state.render_pending = true;
            return;
        }
        if (ch == '2') {
            strlcpy(ble_state.custom_lua_expr,
                    "return string.format('Standard Scale: %.2f kg', ((adv[5] or 0) + (adv[6] or 0)*256)*0.005)",
                    sizeof(ble_state.custom_lua_expr));
            ble_state.adv_mode = ADV_INSPECT_LUA;
            ble_scanner_set_status("Loaded Standard Scale (0x181D) preset");
            ble_state.render_pending = true;
            return;
        }
        if (ch == '3') {
            strlcpy(ble_state.custom_lua_expr,
                    "return string.format('BTHome/Sens: B0=%d B1=%d B2=%d', adv[1] or 0, adv[2] or 0, adv[3] or 0)",
                    sizeof(ble_state.custom_lua_expr));
            ble_state.adv_mode = ADV_INSPECT_LUA;
            ble_scanner_set_status("Loaded BTHome Sensor preset");
            ble_state.render_pending = true;
            return;
        }
        if (ch == ' ' || ch == 'r' || ch == 'R') {
            ble_scanner_start_scan();
            return;
        }
        return;
    }

    if (ble_state.view == BLE_VIEW_INTEGRATED_GATT) {
        if (ch == '\t' || uch == SOLAR_OS_KEY_LEFT || uch == SOLAR_OS_KEY_RIGHT) {
            ble_state.gatt_focus = (ble_state.gatt_focus == GATT_FOCUS_SERVICES) ? GATT_FOCUS_CHARS : GATT_FOCUS_SERVICES;
            ble_state.render_pending = true;
            return;
        }
        if (ble_state.gatt_focus == GATT_FOCUS_SERVICES) {
            if (uch == SOLAR_OS_KEY_UP) {
                if (ble_state.selected_service > 0) {
                    ble_state.selected_service--;
                    ble_load_service_characteristics(ble_state.selected_service);
                }
                return;
            }
            if (uch == SOLAR_OS_KEY_DOWN) {
                if (ble_state.selected_service + 1 < ble_state.service_count) {
                    ble_state.selected_service++;
                    ble_load_service_characteristics(ble_state.selected_service);
                }
                return;
            }
        } else {
            if (uch == SOLAR_OS_KEY_UP) {
                if (ble_state.selected_char > 0) {
                    ble_state.selected_char--;
                    ble_state.has_read_value = false;
                    ble_state.render_pending = true;
                }
                return;
            }
            if (uch == SOLAR_OS_KEY_DOWN) {
                if (ble_state.selected_char + 1 < ble_state.char_count) {
                    ble_state.selected_char++;
                    ble_state.has_read_value = false;
                    ble_state.render_pending = true;
                }
                return;
            }
        }
        if (ch == 'm' || ch == 'M') {
            ble_state.decoder_mode = (gatt_decoder_mode_t)((ble_state.decoder_mode + 1) % GATT_DECODER_COUNT);
            ble_state.render_pending = true;
            return;
        }
        if (ch == '\n' || ch == '\r' || ch == 'r' || ch == 'R') {
            ble_read_selected_characteristic();
            return;
        }
        if (ch == 'd' || ch == 'D') {
            (void)solar_os_ble_gatt_disconnect();
            ble_state.view = BLE_VIEW_SCANNER;
            ble_scanner_set_status("Disconnected");
            ble_state.render_pending = true;
            return;
        }
        return;
    }

    if (ble_state.view == BLE_VIEW_RADAR) {
        if (ch == 'b' || ch == 'B') {
            ble_state.beep_enabled = !ble_state.beep_enabled;
            ble_scanner_set_status(ble_state.beep_enabled ? "Audio Beep ON" : "Audio Beep OFF");
            ble_state.render_pending = true;
            return;
        }
        if (ch == ' ' || ch == 'r' || ch == 'R') {
            ble_scanner_start_scan();
            return;
        }
    }
}

static bool ble_scanner_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    switch (event->type) {
    case SOLAR_OS_EVENT_CHAR:
        ble_scanner_handle_char(ctx, event->data.ch);
        break;

    case SOLAR_OS_EVENT_TICK:
        ble_state.elapsed_ms += BLE_SCANNER_TICK_MS;

        /* Check background scan worker results */
        if (ble_state.scanning) {
            bool ready = false;
            size_t count = 0;
            solar_os_ble_keyboard_scan_result_t staged[BLE_SCANNER_MAX_DEVICES];

            portENTER_CRITICAL(&ble_scanner_lock);
            if (ble_state.staging_ready) {
                ready = true;
                count = ble_state.staging_count;
                memcpy(staged, ble_state.staging_results, count * sizeof(staged[0]));
                ble_state.staging_ready = false;
            }
            portEXIT_CRITICAL(&ble_scanner_lock);

            if (ready) {
                ble_state.scanning = false;

                for (size_t s = 0; s < count; s++) {
                    int existing_idx = -1;
                    for (size_t d = 0; d < ble_state.device_count; d++) {
                        if (memcmp(ble_state.devices[d].bda, staged[s].bda, 6) == 0) {
                            existing_idx = (int)d;
                            break;
                        }
                    }

                    ble_device_item_t *target;
                    if (existing_idx >= 0) {
                        target = &ble_state.devices[existing_idx];
                    } else if (ble_state.device_count < BLE_SCANNER_MAX_DEVICES) {
                        target = &ble_state.devices[ble_state.device_count++];
                        memset(target, 0, sizeof(*target));
                        memcpy(target->bda, staged[s].bda, 6);
                    } else {
                        continue;
                    }

                    target->addr_type = staged[s].addr_type;
                    target->rssi = staged[s].rssi;
                    target->appearance = staged[s].appearance;
                    target->hid_service = staged[s].hid_service;
                    target->keyboard_like = staged[s].keyboard_like;
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

                    /* Radar view update */
                    if (ble_state.view == BLE_VIEW_RADAR &&
                        memcmp(target->bda, ble_state.tracked_bda, 6) == 0) {
                        ble_state.tracked_rssi = target->rssi;
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

        /* Periodic continuous auto-scan: 60s for List, 5s for Radar */
        if (!ble_state.scanning && !ble_state.gatt_connecting && !ble_state.gatt_connected) {
            const uint32_t auto_interval = (ble_state.view == BLE_VIEW_RADAR) ?
                                           BLE_SCANNER_RADAR_RESCAN_MS : BLE_SCANNER_LIST_AUTORESCAN_MS;
            if (ble_state.elapsed_ms >= ble_state.next_auto_rescan_ms) {
                ble_state.next_auto_rescan_ms = ble_state.elapsed_ms + auto_interval;
                ble_scanner_start_scan();
            }
        }

        /* Radar animations & beeps */
        if (ble_state.view == BLE_VIEW_RADAR) {
            ble_state.radar_sweep_angle += 0.15f;
            if (ble_state.radar_sweep_angle > 6.283185f) {
                ble_state.radar_sweep_angle = 0.0f;
            }
            if (ble_state.beep_enabled && ble_state.elapsed_ms >= ble_state.next_beep_ms) {
                int8_t r = ble_state.tracked_rssi;
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
    .tick_deadline_ms = BLE_SCANNER_TICK_MS * 3U,
};
