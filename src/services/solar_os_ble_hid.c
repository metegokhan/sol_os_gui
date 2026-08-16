/*
 * Solar OS - Modular BLE HID Host Service
 * Manages concurrent HID connections (Keyboards, Mice, Gamepads) and routes reports.
 */

#include "solar_os_ble_hid.h"
#include "solar_os_ble_core.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_mouse.h"
#include "solar_os_gamepad.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_hidh.h"
#include "esp_private/esp_hidh_private.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "solar_os_log.h"
#include "solar_os_task.h"

#define TAG "ble_hid"
#define BLE_HID_NVS_NAMESPACE "ble_kb"
#define BLE_HID_NVS_PEERS_KEY "peers"
#define BLE_HID_PEER_MAGIC 0x424CU

typedef struct {
    uint16_t magic;
    uint8_t addr_type;
    uint8_t bda[6];
    char name[32];
} ble_hid_persisted_peer_t;

typedef struct {
    esp_hidh_dev_t *dev;
    uint8_t bda[6];
    esp_ble_addr_type_t addr_type;
    char name[64];
    solar_os_ble_dev_type_t type;
    bool connected;
    uint8_t battery_level;
} ble_hid_slot_t;

static bool s_hid_initialized = false;
static ble_hid_slot_t s_slots[SOLAR_OS_BLE_HID_MAX_CONNECTED] = {0};
static ble_hid_persisted_peer_t s_remembered_peers[SOLAR_OS_BLE_HID_MAX_CONNECTED] = {0};

static SemaphoreHandle_t s_hid_mutex = NULL;
static TaskHandle_t s_reconnect_task_handle = NULL;
static bool s_reconnect_stop_requested = false;

static void hid_lock(void)
{
    if (s_hid_mutex != NULL) {
        xSemaphoreTake(s_hid_mutex, portMAX_DELAY);
    }
}

static void hid_unlock(void)
{
    if (s_hid_mutex != NULL) {
        xSemaphoreGive(s_hid_mutex);
    }
}

#define BLE_KEYBOARD_PEER_MAGIC_LEGACY 0x424B5052U
typedef struct {
    uint32_t magic;
    uint8_t addr_type;
    uint8_t bda[6];
    char name[64];
} ble_keyboard_peer_legacy_t;

static esp_err_t load_remembered_peers(void)
{
    memset(s_remembered_peers, 0, sizeof(s_remembered_peers));
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_HID_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) return ret;

    uint8_t raw_buf[sizeof(ble_keyboard_peer_legacy_t) * 4] = {0};
    size_t raw_size = sizeof(raw_buf);

    ret = nvs_get_blob(nvs, BLE_HID_NVS_PEERS_KEY, raw_buf, &raw_size);
    if (ret == ESP_OK && raw_size > 0) {
        /* Check if current format */
        const ble_hid_persisted_peer_t *p_new = (const ble_hid_persisted_peer_t *)raw_buf;
        if (p_new[0].magic == BLE_HID_PEER_MAGIC) {
            size_t copy_cnt = raw_size / sizeof(ble_hid_persisted_peer_t);
            if (copy_cnt > SOLAR_OS_BLE_HID_MAX_CONNECTED) copy_cnt = SOLAR_OS_BLE_HID_MAX_CONNECTED;
            memcpy(s_remembered_peers, raw_buf, copy_cnt * sizeof(ble_hid_persisted_peer_t));
        } else {
            /* Migrate legacy multi-peer struct */
            const ble_keyboard_peer_legacy_t *p_old = (const ble_keyboard_peer_legacy_t *)raw_buf;
            size_t old_cnt = raw_size / sizeof(ble_keyboard_peer_legacy_t);
            size_t out_idx = 0;
            for (size_t i = 0; i < old_cnt && out_idx < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
                if (p_old[i].magic == BLE_KEYBOARD_PEER_MAGIC_LEGACY) {
                    s_remembered_peers[out_idx].magic = BLE_HID_PEER_MAGIC;
                    s_remembered_peers[out_idx].addr_type = p_old[i].addr_type;
                    memcpy(s_remembered_peers[out_idx].bda, p_old[i].bda, 6);
                    strlcpy(s_remembered_peers[out_idx].name, p_old[i].name[0] ? p_old[i].name : "Keyboard", sizeof(s_remembered_peers[out_idx].name));
                    out_idx++;
                }
            }
        }
    } else {
        /* Check single legacy peer */
        ble_keyboard_peer_legacy_t legacy_peer;
        size_t leg_size = sizeof(legacy_peer);
        if (nvs_get_blob(nvs, "peer", &legacy_peer, &leg_size) == ESP_OK) {
            if (legacy_peer.magic == BLE_KEYBOARD_PEER_MAGIC_LEGACY) {
                s_remembered_peers[0].magic = BLE_HID_PEER_MAGIC;
                s_remembered_peers[0].addr_type = legacy_peer.addr_type;
                memcpy(s_remembered_peers[0].bda, legacy_peer.bda, 6);
                strlcpy(s_remembered_peers[0].name, legacy_peer.name[0] ? legacy_peer.name : "Keyboard", sizeof(s_remembered_peers[0].name));
            }
        }
    }
    nvs_close(nvs);

    for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
        if (s_remembered_peers[i].magic == BLE_HID_PEER_MAGIC) {
            SOLAR_OS_LOGI(TAG, "Loaded remembered peer: " ESP_BD_ADDR_STR " (%s, type=%u)",
                          ESP_BD_ADDR_HEX(s_remembered_peers[i].bda),
                          s_remembered_peers[i].name,
                          (unsigned)s_remembered_peers[i].addr_type);
        }
    }
    return ESP_OK;
}

static esp_err_t save_remembered_peers(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_HID_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_blob(nvs, BLE_HID_NVS_PEERS_KEY, s_remembered_peers, sizeof(s_remembered_peers));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void record_remembered_peer(const uint8_t *bda, esp_ble_addr_type_t addr_type, const char *name)
{
    if (bda == NULL) return;
    for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
        if (s_remembered_peers[i].magic == BLE_HID_PEER_MAGIC &&
            memcmp(s_remembered_peers[i].bda, bda, 6) == 0) {
            s_remembered_peers[i].addr_type = (uint8_t)addr_type;
            strlcpy(s_remembered_peers[i].name, name != NULL ? name : "Device", sizeof(s_remembered_peers[i].name));
            (void)save_remembered_peers();
            return;
        }
    }
    for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
        if (s_remembered_peers[i].magic != BLE_HID_PEER_MAGIC) {
            s_remembered_peers[i].magic = BLE_HID_PEER_MAGIC;
            s_remembered_peers[i].addr_type = (uint8_t)addr_type;
            memcpy(s_remembered_peers[i].bda, bda, 6);
            strlcpy(s_remembered_peers[i].name, name != NULL ? name : "Device", sizeof(s_remembered_peers[i].name));
            (void)save_remembered_peers();
            return;
        }
    }
}

static size_t remembered_peer_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
        if (s_remembered_peers[i].magic == BLE_HID_PEER_MAGIC) {
            count++;
        }
    }
    return count;
}

size_t solar_os_ble_hid_remembered_count(void)
{
    return remembered_peer_count();
}

esp_err_t solar_os_ble_hid_forget_all(void)
{
    memset(s_remembered_peers, 0, sizeof(s_remembered_peers));
    nvs_handle_t nvs;
    if (nvs_open(BLE_HID_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_erase_key(nvs, BLE_HID_NVS_PEERS_KEY);
        (void)nvs_erase_key(nvs, "peer");
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
    SOLAR_OS_LOGI(TAG, "All remembered HID peers cleared");
    return ESP_OK;
}

static void update_device_subsystem_states(void)
{
    bool any_mouse = false;
    bool any_gamepad = false;
    bool any_keyboard = false;

    for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
        if (s_slots[i].connected && s_slots[i].dev != NULL) {
            if (s_slots[i].type == SOLAR_OS_BLE_DEV_TYPE_MOUSE) any_mouse = true;
            if (s_slots[i].type == SOLAR_OS_BLE_DEV_TYPE_GAMEPAD) any_gamepad = true;
            if (s_slots[i].type == SOLAR_OS_BLE_DEV_TYPE_KEYBOARD) any_keyboard = true;
        }
    }

    solar_os_mouse_set_connected(any_mouse);
    solar_os_gamepad_set_connected(any_gamepad);
    if (!any_keyboard) {
        solar_os_ble_keyboard_reset_state(false);
    }
}

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    if (event_data == NULL) return;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.status == ESP_OK) {
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            const char *name = esp_hidh_dev_name_get(param->open.dev);
            if (name == NULL || name[0] == '\0') name = "HID Device";

            esp_hid_usage_t usage = esp_hidh_dev_usage_get(param->open.dev);
            solar_os_ble_dev_type_t dev_type = SOLAR_OS_BLE_DEV_TYPE_KEYBOARD;
            if (usage == ESP_HID_USAGE_MOUSE || solar_os_ble_is_mouse_like(0, name)) {
                dev_type = SOLAR_OS_BLE_DEV_TYPE_MOUSE;
            } else if (usage == ESP_HID_USAGE_JOYSTICK || usage == ESP_HID_USAGE_GAMEPAD || solar_os_ble_is_gamepad_like(0, name)) {
                dev_type = SOLAR_OS_BLE_DEV_TYPE_GAMEPAD;
            }

            hid_lock();
            for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
                if (!s_slots[i].connected || (bda != NULL && memcmp(s_slots[i].bda, bda, 6) == 0)) {
                    s_slots[i].dev = param->open.dev;
                    if (bda != NULL) memcpy(s_slots[i].bda, bda, 6);
                    strlcpy(s_slots[i].name, name, sizeof(s_slots[i].name));
                    s_slots[i].type = dev_type;
                    s_slots[i].connected = true;
                    s_slots[i].battery_level = 100;
                    break;
                }
            }
            update_device_subsystem_states();
            hid_unlock();

            if (bda != NULL) {
                esp_ble_addr_type_t dev_addr_type = BLE_ADDR_TYPE_RANDOM;
                if (param->open.dev != NULL) {
                    dev_addr_type = param->open.dev->ble.address_type;
                }
                record_remembered_peer(bda, dev_addr_type, name);
                SOLAR_OS_LOGI(TAG, "HID device connected: %s [" ESP_BD_ADDR_STR "] (type %d, addr_type=%u)",
                              name, ESP_BD_ADDR_HEX(bda), (int)dev_type, (unsigned)dev_addr_type);
            }
        } else {
            SOLAR_OS_LOGW(TAG, "HID open failed: %s", esp_err_to_name(param->open.status));
        }
        break;
    }

    case ESP_HIDH_INPUT_EVENT: {
        if (param->input.usage == ESP_HID_USAGE_KEYBOARD) {
            solar_os_ble_keyboard_process_report(param->input.map_index,
                                                 param->input.report_id,
                                                 param->input.data,
                                                 param->input.length);
        } else if (param->input.usage == ESP_HID_USAGE_MOUSE) {
            const uint8_t *m_data = param->input.data;
            uint16_t m_len = param->input.length;
            if (param->input.report_id != 0 && m_len >= 5 && m_data[0] == (uint8_t)param->input.report_id) {
                m_data++;
                m_len--;
            }
            if (m_len >= 3) {
                uint8_t btns = m_data[0];
                int8_t dx = (int8_t)m_data[1];
                int8_t dy = (int8_t)m_data[2];
                int8_t wheel = (m_len >= 4) ? (int8_t)m_data[3] : 0;
                solar_os_mouse_process_report(btns, dx, dy, wheel);
            }
        } else if (param->input.usage == ESP_HID_USAGE_JOYSTICK || param->input.usage == ESP_HID_USAGE_GAMEPAD) {
            solar_os_gamepad_process_report(param->input.data, param->input.length);
        } else {
            /* Heuristic fallback */
            const char *dev_name = esp_hidh_dev_name_get(param->input.dev);
            if (dev_name != NULL && solar_os_ble_is_mouse_like(0, dev_name)) {
                const uint8_t *m_data = param->input.data;
                uint16_t m_len = param->input.length;
                if (param->input.report_id != 0 && m_len >= 5 && m_data[0] == (uint8_t)param->input.report_id) {
                    m_data++;
                    m_len--;
                }
                if (m_len >= 3) {
                    uint8_t btns = m_data[0];
                    int8_t dx = (int8_t)m_data[1];
                    int8_t dy = (int8_t)m_data[2];
                    int8_t wheel = (m_len >= 4) ? (int8_t)m_data[3] : 0;
                    solar_os_mouse_process_report(btns, dx, dy, wheel);
                }
            } else if (dev_name != NULL && solar_os_ble_is_gamepad_like(0, dev_name)) {
                solar_os_gamepad_process_report(param->input.data, param->input.length);
            } else {
                solar_os_ble_keyboard_process_consumer_report(param->input.report_id, param->input.data, param->input.length);
            }
        }
        break;
    }

    case ESP_HIDH_BATTERY_EVENT: {
        hid_lock();
        for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
            if (s_slots[i].dev == param->battery.dev) {
                s_slots[i].battery_level = param->battery.level;
                break;
            }
        }
        hid_unlock();
        break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
        hid_lock();
        for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
            if (s_slots[i].dev == param->close.dev) {
                s_slots[i].connected = false;
                s_slots[i].dev = NULL;
                break;
            }
        }
        update_device_subsystem_states();
        hid_unlock();

        esp_hidh_dev_free(param->close.dev);
        SOLAR_OS_LOGI(TAG, "HID device disconnected");
        break;
    }

    default:
        break;
    }
}

static TaskHandle_t s_pairing_task_handle = NULL;

static void hid_pairing_task(void *arg)
{
    (void)arg;
    SOLAR_OS_LOGI(TAG, "Starting BLE HID auto-pairing scan (5s)...");

    solar_os_ble_scan_item_t scan_results[16] = {0};
    size_t found_count = 0;

    esp_err_t scan_err = solar_os_ble_scan_start(5, scan_results, 16, &found_count);
    if (scan_err != ESP_OK || found_count == 0) {
        SOLAR_OS_LOGW(TAG, "Pairing scan found no devices or failed: %s", esp_err_to_name(scan_err));
        s_pairing_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    int best_idx = -1;
    int8_t best_rssi = -128;

    for (size_t i = 0; i < found_count; i++) {
        if (solar_os_ble_hid_is_connected(scan_results[i].bda)) continue;

        bool is_hid = scan_results[i].hid_service ||
                      scan_results[i].keyboard_like ||
                      scan_results[i].mouse_like ||
                      scan_results[i].gamepad_like ||
                      scan_results[i].appearance == 0x03C1 ||
                      scan_results[i].appearance == 0x03C2 ||
                      scan_results[i].appearance == 0x03C4 ||
                      solar_os_ble_is_hid_like(scan_results[i].appearance, scan_results[i].name);

        if (is_hid) {
            if (best_idx == -1 || scan_results[i].rssi > best_rssi) {
                best_idx = (int)i;
                best_rssi = scan_results[i].rssi;
            }
        }
    }

    if (best_idx >= 0) {
        SOLAR_OS_LOGI(TAG, "Auto-pairing candidate found: %s [" ESP_BD_ADDR_STR "] (rssi=%d, type=%u)",
                      scan_results[best_idx].name[0] ? scan_results[best_idx].name : "HID Device",
                      ESP_BD_ADDR_HEX(scan_results[best_idx].bda),
                      scan_results[best_idx].rssi,
                      (unsigned)scan_results[best_idx].addr_type);

        (void)solar_os_ble_hid_connect(scan_results[best_idx].bda,
                                      scan_results[best_idx].addr_type,
                                      scan_results[best_idx].name);
    } else {
        SOLAR_OS_LOGW(TAG, "No suitable BLE HID device detected in pairing mode during scan");
    }

    s_pairing_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t solar_os_ble_hid_start_pairing(void)
{
    if (s_pairing_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t ret = xTaskCreate(hid_pairing_task, "ble_pair", 8192, NULL, tskIDLE_PRIORITY + 2, &s_pairing_task_handle);
    return ret == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static solar_os_ble_scan_item_t s_reconn_scan_results[4];

static void hid_reconnect_task(void *arg)
{
    (void)arg;
    /* Allow boot, launcher, and wifi to stabilize */
    vTaskDelay(pdMS_TO_TICKS(8000));

    while (!s_reconnect_stop_requested) {
        if (s_reconnect_stop_requested) break;

        /* Do not interfere if user is scanning or pairing in foreground */
        if (!solar_os_ble_is_scanning() && s_pairing_task_handle == NULL) {
            /* Check if any remembered peer is disconnected */
            bool has_disconnected_peer = false;
            for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
                if (s_remembered_peers[i].magic == BLE_HID_PEER_MAGIC &&
                    !solar_os_ble_hid_is_connected(s_remembered_peers[i].bda)) {
                    has_disconnected_peer = true;
                    break;
                }
            }

            if (has_disconnected_peer) {
                size_t found = 0;
                esp_err_t scan_err = solar_os_ble_scan_start(2, s_reconn_scan_results, 4, &found);
                if (scan_err == ESP_OK && found > 0 && !s_reconnect_stop_requested) {
                    for (size_t i = 0; i < found; i++) {
                        for (size_t p = 0; p < SOLAR_OS_BLE_HID_MAX_CONNECTED; p++) {
                            if (s_remembered_peers[p].magic == BLE_HID_PEER_MAGIC &&
                                memcmp(s_remembered_peers[p].bda, s_reconn_scan_results[i].bda, 6) == 0 &&
                                !solar_os_ble_hid_is_connected(s_reconn_scan_results[i].bda)) {
                                SOLAR_OS_LOGI(TAG, "Auto-reconnecting remembered device: %s [" ESP_BD_ADDR_STR "]",
                                              s_remembered_peers[p].name, ESP_BD_ADDR_HEX(s_reconn_scan_results[i].bda));
                                (void)solar_os_ble_hid_connect(s_reconn_scan_results[i].bda,
                                                              s_reconn_scan_results[i].addr_type,
                                                              s_remembered_peers[p].name);
                                break;
                            }
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(15000));
    }
    s_reconnect_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t solar_os_ble_hid_init(void)
{
    if (s_hid_initialized) return ESP_OK;

    ESP_LOGI(TAG, "solar_os_ble_hid_init calling solar_os_ble_core_init...");
    esp_err_t ret = solar_os_ble_core_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "solar_os_ble_core_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_hid_mutex == NULL) {
        s_hid_mutex = xSemaphoreCreateMutex();
    }

    ESP_LOGI(TAG, "Loading remembered peers...");
    (void)load_remembered_peers();

    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 6144,
        .callback_arg = NULL,
    };

    static bool s_hidh_inited = false;
    if (!s_hidh_inited) {
        ESP_LOGI(TAG, "Calling esp_hidh_init...");
        ret = esp_hidh_init(&config);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_hidh_init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_hidh_inited = true;
    }

    /* Initialize input devices */
    ESP_LOGI(TAG, "Initializing keyboard, mouse, gamepad drivers...");
    (void)solar_os_ble_keyboard_init();
    (void)solar_os_mouse_init();
    (void)solar_os_gamepad_init();

    s_hid_initialized = true;

    /* Start background reconnect task only if there are remembered peers */
    if (remembered_peer_count() > 0 && s_reconnect_task_handle == NULL) {
        s_reconnect_stop_requested = false;
        (void)xTaskCreate(hid_reconnect_task, "ble_reconn", 6144, NULL, tskIDLE_PRIORITY + 1, &s_reconnect_task_handle);
    }

    ESP_LOGI(TAG, "BLE HID Host initialized successfully");
    return ESP_OK;
}

size_t solar_os_ble_hid_connected_count(void)
{
    size_t count = 0;
    hid_lock();
    for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
        if (s_slots[i].connected && s_slots[i].dev != NULL) {
            count++;
        }
    }
    hid_unlock();
    return count;
}

bool solar_os_ble_hid_get_connected_dev(size_t index, solar_os_ble_connected_dev_info_t *out)
{
    if (out == NULL) return false;
    hid_lock();
    size_t cur = 0;
    for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
        if (s_slots[i].connected && s_slots[i].dev != NULL) {
            if (cur == index) {
                memcpy(out->bda, s_slots[i].bda, 6);
                out->addr_type = (uint8_t)s_slots[i].addr_type;
                strlcpy(out->name, s_slots[i].name, sizeof(out->name));
                out->type = s_slots[i].type;
                out->connected = true;
                out->battery_level = s_slots[i].battery_level;
                hid_unlock();
                return true;
            }
            cur++;
        }
    }
    hid_unlock();
    return false;
}

bool solar_os_ble_hid_is_connected(const uint8_t bda[6])
{
    if (bda == NULL) return false;
    hid_lock();
    for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
        if (s_slots[i].connected && s_slots[i].dev != NULL && memcmp(s_slots[i].bda, bda, 6) == 0) {
            hid_unlock();
            return true;
        }
    }
    hid_unlock();
    return false;
}

bool solar_os_ble_hid_has_keyboard(void)
{
    hid_lock();
    for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
        if (s_slots[i].connected && s_slots[i].dev != NULL && s_slots[i].type == SOLAR_OS_BLE_DEV_TYPE_KEYBOARD) {
            hid_unlock();
            return true;
        }
    }
    hid_unlock();
    return false;
}

esp_err_t solar_os_ble_hid_connect(const uint8_t bda[6], uint8_t addr_type, const char *name)
{
    if (bda == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = solar_os_ble_hid_init();
    if (ret != ESP_OK) return ret;

    (void)solar_os_ble_scan_stop();

    hid_lock();
    for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
        if (s_slots[i].connected && s_slots[i].dev != NULL && memcmp(s_slots[i].bda, bda, 6) == 0) {
            esp_hidh_dev_t *d = s_slots[i].dev;
            s_slots[i].connected = false;
            s_slots[i].dev = NULL;
            (void)esp_hidh_dev_close(d);
            break;
        }
    }
    hid_unlock();

    SOLAR_OS_LOGI(TAG, "Opening BLE HID device: " ESP_BD_ADDR_STR " (addr_type=%u, name=%s)",
                  ESP_BD_ADDR_HEX(bda), (unsigned)addr_type, name ? name : "");

    esp_hidh_dev_t *dev = esp_hidh_dev_open((uint8_t *)bda, ESP_HID_TRANSPORT_BLE, (esp_ble_addr_type_t)addr_type);
    if (dev == NULL) {
        esp_ble_addr_type_t alt_type = (addr_type == BLE_ADDR_TYPE_PUBLIC) ? BLE_ADDR_TYPE_RANDOM : BLE_ADDR_TYPE_PUBLIC;
        SOLAR_OS_LOGW(TAG, "esp_hidh_dev_open failed with type %u, retrying with alternative type %u...",
                      (unsigned)addr_type, (unsigned)alt_type);
        dev = esp_hidh_dev_open((uint8_t *)bda, ESP_HID_TRANSPORT_BLE, alt_type);
        if (dev != NULL) {
            addr_type = (uint8_t)alt_type;
        }
    }

    if (dev == NULL) {
        SOLAR_OS_LOGE(TAG, "esp_hidh_dev_open failed for all address types");
        return ESP_FAIL;
    }

    record_remembered_peer(bda, (esp_ble_addr_type_t)addr_type, name);
    return ESP_OK;
}

esp_err_t solar_os_ble_hid_disconnect(const uint8_t bda[6])
{
    if (bda == NULL) return ESP_ERR_INVALID_ARG;
    hid_lock();
    for (size_t i = 0; i < SOLAR_OS_BLE_HID_MAX_CONNECTED; i++) {
        if (s_slots[i].connected && s_slots[i].dev != NULL && memcmp(s_slots[i].bda, bda, 6) == 0) {
            esp_hidh_dev_t *d = s_slots[i].dev;
            s_slots[i].connected = false;
            s_slots[i].dev = NULL;
            hid_unlock();
            (void)esp_hidh_dev_close(d);
            return ESP_OK;
        }
    }
    hid_unlock();
    return ESP_ERR_NOT_FOUND;
}

void solar_os_ble_hid_get_status(char *out_status, size_t max_len)
{
    if (out_status == NULL || max_len == 0) return;
    const size_t cnt = solar_os_ble_hid_connected_count();
    if (cnt == 0) {
        strlcpy(out_status, "No HID devices connected", max_len);
    } else {
        snprintf(out_status, max_len, "%u HID device(s) connected", (unsigned)cnt);
    }
}
