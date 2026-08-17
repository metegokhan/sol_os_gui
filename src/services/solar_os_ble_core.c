/*
 * Solar OS - Central BLE Core Service
 * Manages BT Controller, Bluedroid, GAP event routing, scan serialization, and security.
 */

#include "solar_os_ble_core.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_heap_caps.h"
#include "esp_hidh_gattc.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "solar_os_log.h"
#include "solar_os_power.h"

#define TAG "ble_core"
#define BLE_NVS_NAMESPACE "ble_kb"
#define BLE_NVS_ENABLED_KEY "enabled"

#define BLE_APPEARANCE_HID_KEYBOARD 0x03C1
#define BLE_APPEARANCE_HID_MOUSE    0x03C2
#define BLE_APPEARANCE_HID_JOYSTICK 0x03C3
#define BLE_APPEARANCE_HID_GAMEPAD  0x03C4

static bool s_initialized = false;
static bool s_boot_policy_loaded = false;
static bool s_enabled_for_current_boot = true;
static bool s_enabled_for_next_boot = true;
static bool s_disabled_boot_memory_released = false;

static SemaphoreHandle_t s_scan_mutex = NULL;
static SemaphoreHandle_t s_scan_done_sem = NULL;
static bool s_is_scanning = false;

static solar_os_ble_scan_item_t *s_active_scan_results = NULL;
static size_t s_active_scan_max = 0;
static size_t s_active_scan_count = 0;

static solar_os_ble_gap_event_cb_t s_gap_callbacks[SOLAR_OS_BLE_MAX_GAP_CALLBACKS] = {0};
static solar_os_ble_gattc_event_cb_t s_gattc_callbacks[SOLAR_OS_BLE_MAX_GATTC_CALLBACKS] = {0};

/* Forward declarations */
static void central_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void central_gattc_callback(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static esp_err_t central_init_security(void);

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
        if (ret == ESP_ERR_INVALID_STATE) {
            return ESP_OK;
        }
    }
    return ret;
}

static esp_err_t load_boot_policy(void)
{
    if (s_boot_policy_loaded) {
        return ESP_OK;
    }

    esp_err_t ret = init_nvs();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(BLE_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        s_enabled_for_current_boot = true;
        s_enabled_for_next_boot = true;
        s_boot_policy_loaded = true;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t stored = 1U;
    ret = nvs_get_u8(nvs, BLE_NVS_ENABLED_KEY, &stored);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        s_enabled_for_current_boot = true;
        s_enabled_for_next_boot = true;
        s_boot_policy_loaded = true;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    s_enabled_for_current_boot = stored != 0U;
    s_enabled_for_next_boot = s_enabled_for_current_boot;
    s_boot_policy_loaded = true;
    return ESP_OK;
}

bool solar_os_ble_core_enabled_for_current_boot(void)
{
    (void)load_boot_policy();
    return s_enabled_for_current_boot;
}

bool solar_os_ble_core_enabled_for_next_boot(void)
{
    (void)load_boot_policy();
    return s_enabled_for_next_boot;
}

esp_err_t solar_os_ble_core_set_enabled_for_next_boot(bool enabled)
{
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");
    (void)load_boot_policy();

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_u8(nvs, BLE_NVS_ENABLED_KEY, enabled ? 1U : 0U);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        s_enabled_for_next_boot = enabled;
    }
    return ret;
}

esp_err_t solar_os_ble_core_apply_boot_policy(void)
{
    if (solar_os_ble_core_enabled_for_current_boot()) {
        return ESP_OK;
    }
    if (s_disabled_boot_memory_released) {
        return ESP_OK;
    }
    s_disabled_boot_memory_released = true;

#if defined(SOC_BT_CLASSIC_SUPPORTED) && SOC_BT_CLASSIC_SUPPORTED
    const esp_bt_mode_t release_mode = ESP_BT_MODE_BTDM;
#else
    const esp_bt_mode_t release_mode = ESP_BT_MODE_BLE;
#endif

    esp_err_t ret = esp_bt_mem_release(release_mode);
    if (ret == ESP_OK) {
        SOLAR_OS_LOGI(TAG, "BLE disabled for this boot; memory released");
    }
    return ret;
}

esp_err_t solar_os_ble_register_gap_callback(solar_os_ble_gap_event_cb_t cb)
{
    if (cb == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < SOLAR_OS_BLE_MAX_GAP_CALLBACKS; i++) {
        if (s_gap_callbacks[i] == cb) return ESP_OK;
    }
    for (size_t i = 0; i < SOLAR_OS_BLE_MAX_GAP_CALLBACKS; i++) {
        if (s_gap_callbacks[i] == NULL) {
            s_gap_callbacks[i] = cb;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t solar_os_ble_unregister_gap_callback(solar_os_ble_gap_event_cb_t cb)
{
    if (cb == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < SOLAR_OS_BLE_MAX_GAP_CALLBACKS; i++) {
        if (s_gap_callbacks[i] == cb) {
            s_gap_callbacks[i] = NULL;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_ble_register_gattc_callback(solar_os_ble_gattc_event_cb_t cb)
{
    if (cb == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < SOLAR_OS_BLE_MAX_GATTC_CALLBACKS; i++) {
        if (s_gattc_callbacks[i] == cb) return ESP_OK;
    }
    for (size_t i = 0; i < SOLAR_OS_BLE_MAX_GATTC_CALLBACKS; i++) {
        if (s_gattc_callbacks[i] == NULL) {
            s_gattc_callbacks[i] = cb;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t solar_os_ble_unregister_gattc_callback(solar_os_ble_gattc_event_cb_t cb)
{
    if (cb == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < SOLAR_OS_BLE_MAX_GATTC_CALLBACKS; i++) {
        if (s_gattc_callbacks[i] == cb) {
            s_gattc_callbacks[i] = NULL;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static void central_gattc_callback(esp_gattc_cb_event_t event,
                                   esp_gatt_if_t gattc_if,
                                   esp_ble_gattc_cb_param_t *param)
{
    /* Forward unconditionally to ESP-IDF HID Host */
    esp_hidh_gattc_event_handler(event, gattc_if, param);

    /* Dispatch to registered module callbacks */
    for (size_t i = 0; i < SOLAR_OS_BLE_MAX_GATTC_CALLBACKS; i++) {
        if (s_gattc_callbacks[i] != NULL) {
            s_gattc_callbacks[i](event, gattc_if, param);
        }
    }
}

static esp_err_t central_init_security(void)
{
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK | ESP_BLE_CSR_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK | ESP_BLE_CSR_KEY_MASK;
    uint8_t oob_support = ESP_BLE_OOB_DISABLE;

    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req)),
                        TAG, "set auth req failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap)),
                        TAG, "set io cap failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)),
                        TAG, "set key size failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_OOB_SUPPORT, &oob_support, sizeof(oob_support)),
                        TAG, "set oob support failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key)),
                        TAG, "set init key failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key)),
                        TAG, "set rsp key failed");

    return ESP_OK;
}

esp_err_t solar_os_ble_core_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!solar_os_ble_core_enabled_for_current_boot()) {
        (void)solar_os_ble_core_apply_boot_policy();
        return ESP_ERR_NOT_ALLOWED;
    }

    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");

    if (s_scan_mutex == NULL) {
        s_scan_mutex = xSemaphoreCreateMutex();
    }
    if (s_scan_done_sem == NULL) {
        s_scan_done_sem = xSemaphoreCreateBinary();
    }

    ESP_LOGI(TAG, "BLE core init start");

    static bool s_classic_bt_released = false;
    if (!s_classic_bt_released) {
        esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
            s_classic_bt_released = true;
        } else {
            ESP_LOGW(TAG, "classic bt memory release failed: %s", esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "BLE controller init...");
    esp_bt_controller_status_t controller_status = esp_bt_controller_get_status();
    if (controller_status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_bt_controller_init(&bt_cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
            return err;
        }
        controller_status = esp_bt_controller_get_status();
    }
    ESP_LOGI(TAG, "BLE controller enable...");
    if (controller_status == ESP_BT_CONTROLLER_STATUS_INITED) {
        esp_err_t err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "Bluedroid init...");
    esp_bluedroid_status_t bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
        bluedroid_cfg.ssp_en = false;
        esp_err_t err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_bluedroid_init_with_cfg failed: %s", esp_err_to_name(err));
            return err;
        }
        bluedroid_status = esp_bluedroid_get_status();
    }
    ESP_LOGI(TAG, "Bluedroid enable...");
    if (bluedroid_status == ESP_BLUEDROID_STATUS_INITIALIZED) {
        esp_err_t err = esp_bluedroid_enable();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "GAP register callback...");
    esp_err_t gap_err = esp_ble_gap_register_callback(central_gap_callback);
    if (gap_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_register_callback failed: %s", esp_err_to_name(gap_err));
        return gap_err;
    }

    ESP_LOGI(TAG, "GATTC register callback...");
    esp_err_t gattc_err = esp_ble_gattc_register_callback(central_gattc_callback);
    if (gattc_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gattc_register_callback failed: %s", esp_err_to_name(gattc_err));
        return gattc_err;
    }

    ESP_LOGI(TAG, "Central init security...");
    (void)central_init_security();

    s_initialized = true;
    ESP_LOGI(TAG, "BLE Core initialized successfully");
    return ESP_OK;
}

bool solar_os_ble_core_is_initialized(void)
{
    return s_initialized;
}

/* ---------------------------------------------------------------------
 * Helper functions
 * ------------------------------------------------------------------- */

static bool contains_ci(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL) return false;
    const size_t needle_len = strlen(needle);
    if (needle_len == 0) return true;
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            if (strncasecmp(haystack, needle, needle_len) == 0) return true;
        }
    }
    return false;
}

bool solar_os_ble_is_keyboard_like(uint16_t appearance, const char *name)
{
    if (appearance == BLE_APPEARANCE_HID_KEYBOARD) return true;
    return contains_ci(name, "keyboard") || contains_ci(name, "kbd") || contains_ci(name, "keychron");
}

bool solar_os_ble_is_mouse_like(uint16_t appearance, const char *name)
{
    if (appearance == BLE_APPEARANCE_HID_MOUSE) return true;
    return contains_ci(name, "mouse") || contains_ci(name, "trackball") ||
           contains_ci(name, "touchpad") || contains_ci(name, "pointer");
}

bool solar_os_ble_is_gamepad_like(uint16_t appearance, const char *name)
{
    if (appearance == BLE_APPEARANCE_HID_JOYSTICK || appearance == BLE_APPEARANCE_HID_GAMEPAD) return true;
    return contains_ci(name, "xbox") || contains_ci(name, "switch") || contains_ci(name, "joy-con") ||
           contains_ci(name, "pro controller") || contains_ci(name, "gamepad") ||
           contains_ci(name, "controller") || contains_ci(name, "8bitdo") ||
           contains_ci(name, "dualsense") || contains_ci(name, "dualshock");
}

bool solar_os_ble_is_hid_like(uint16_t appearance, const char *name)
{
    return solar_os_ble_is_keyboard_like(appearance, name) ||
           solar_os_ble_is_mouse_like(appearance, name) ||
           solar_os_ble_is_gamepad_like(appearance, name);
}

uint16_t solar_os_ble_parse_appearance(const uint8_t *adv_data, uint16_t adv_len)
{
    if (adv_data == NULL || adv_len < 4) return 0;
    uint16_t offset = 0;
    while (offset + 1 < adv_len) {
        const uint8_t length = adv_data[offset];
        if (length == 0 || (offset + 1 + length) > adv_len) break;
        const uint8_t type = adv_data[offset + 1];
        if (type == ESP_BLE_AD_TYPE_APPEARANCE && length >= 3) {
            return (uint16_t)adv_data[offset + 2] | ((uint16_t)adv_data[offset + 3] << 8);
        }
        offset += (uint16_t)(1 + length);
    }
    return 0;
}

bool solar_os_ble_parse_has_uuid16(const uint8_t *adv_data, uint16_t adv_len, uint8_t ad_type, uint16_t target_uuid)
{
    if (adv_data == NULL || adv_len < 3) return false;
    uint16_t offset = 0;
    while (offset + 1 < adv_len) {
        const uint8_t length = adv_data[offset];
        if (length == 0 || (offset + 1 + length) > adv_len) break;
        const uint8_t type = adv_data[offset + 1];
        if (type == ad_type) {
            for (uint8_t i = 0; i + 1 < (length - 1); i += 2) {
                const uint16_t uuid = (uint16_t)adv_data[offset + 2 + i] | ((uint16_t)adv_data[offset + 3 + i] << 8);
                if (uuid == target_uuid) return true;
            }
        }
        offset += (uint16_t)(1 + length);
    }
    return false;
}

void solar_os_ble_parse_name(const uint8_t *adv_data, uint16_t adv_len, char *name_out, size_t max_len)
{
    if (name_out == NULL || max_len == 0) return;
    name_out[0] = '\0';
    if (adv_data == NULL || adv_len < 2) return;

    uint8_t name_len = 0;
    const uint8_t *raw_name = esp_ble_resolve_adv_data((uint8_t *)adv_data, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if (raw_name == NULL || name_len == 0) {
        raw_name = esp_ble_resolve_adv_data((uint8_t *)adv_data, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }
    if (raw_name != NULL && name_len > 0) {
        const size_t copy = name_len < (max_len - 1) ? name_len : (max_len - 1);
        memcpy(name_out, raw_name, copy);
        name_out[copy] = '\0';
    }
}

/* ---------------------------------------------------------------------
 * Central GAP Callback & Scanner
 * ------------------------------------------------------------------- */

static solar_os_ble_scan_item_t *find_scan_item_slot(const uint8_t *bda)
{
    if (s_active_scan_results == NULL || s_active_scan_max == 0 || bda == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < s_active_scan_count; i++) {
        if (memcmp(s_active_scan_results[i].bda, bda, 6) == 0) {
            return &s_active_scan_results[i];
        }
    }
    if (s_active_scan_count >= s_active_scan_max) {
        return NULL;
    }
    return &s_active_scan_results[s_active_scan_count++];
}

static void central_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (param == NULL) return;

    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (s_scan_done_sem != NULL) {
            xSemaphoreGive(s_scan_done_sem);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        switch (param->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT: {
            if (s_is_scanning && s_active_scan_results != NULL) {
                const uint16_t adv_len = param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len;
                const uint8_t *adv_data = (const uint8_t *)param->scan_rst.ble_adv;
                const uint16_t appearance = solar_os_ble_parse_appearance(adv_data, adv_len);
                char name[SOLAR_OS_BLE_NAME_MAX];
                solar_os_ble_parse_name(adv_data, adv_len, name, sizeof(name));

                const bool has_hid =
                    solar_os_ble_parse_has_uuid16(adv_data, adv_len, ESP_BLE_AD_TYPE_16SRV_CMPL, ESP_GATT_UUID_HID_SVC) ||
                    solar_os_ble_parse_has_uuid16(adv_data, adv_len, ESP_BLE_AD_TYPE_16SRV_PART, ESP_GATT_UUID_HID_SVC);

                solar_os_ble_scan_item_t *slot = find_scan_item_slot(param->scan_rst.bda);
                if (slot != NULL) {
                    memcpy(slot->bda, param->scan_rst.bda, 6);
                    slot->addr_type = param->scan_rst.ble_addr_type;
                    slot->rssi = param->scan_rst.rssi;
                    slot->appearance = appearance;
                    slot->hid_service = slot->hid_service || has_hid;
                    slot->keyboard_like = slot->keyboard_like || solar_os_ble_is_keyboard_like(appearance, name);
                    slot->mouse_like = slot->mouse_like || solar_os_ble_is_mouse_like(appearance, name);
                    slot->gamepad_like = slot->gamepad_like || solar_os_ble_is_gamepad_like(appearance, name);
                    if (name[0] != '\0') {
                        strlcpy(slot->name, name, sizeof(slot->name));
                    }
                    if (adv_data != NULL && param->scan_rst.adv_data_len > 0) {
                        const size_t copy = param->scan_rst.adv_data_len < sizeof(slot->adv_data) ?
                                            param->scan_rst.adv_data_len : sizeof(slot->adv_data);
                        memcpy(slot->adv_data, adv_data, copy);
                        slot->adv_data_len = (uint8_t)copy;
                    }
                }
            }
            break;
        }

        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            s_is_scanning = false;
            if (s_scan_done_sem != NULL) {
                xSemaphoreGive(s_scan_done_sem);
            }
            break;

        default:
            break;
        }
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        SOLAR_OS_LOGI(TAG, "security request -> replying OK");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        SOLAR_OS_LOGI(TAG, "numeric comparison: auto-accepting passkey %" PRIu32, param->ble_security.key_notif.passkey);
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT: {
        uint32_t passkey = 123456;
        SOLAR_OS_LOGI(TAG, "passkey requested -> replying %06" PRIu32, passkey);
        esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, passkey);
        break;
    }

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        SOLAR_OS_LOGW(TAG, "Passkey notification: %06" PRIu32 " (type this on the keyboard and press Enter!)", param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_KEY_EVT:
        SOLAR_OS_LOGI(TAG, "SMP Key exchanged (type=0x%x)", param->ble_security.ble_key.key_type);
        break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        SOLAR_OS_LOGI(TAG,
                      "conn params updated [" ESP_BD_ADDR_STR "] status=%d conn_int=%u (%.2fms) latency=%u timeout=%ums",
                      ESP_BD_ADDR_HEX(param->update_conn_params.bda),
                      (int)param->update_conn_params.status,
                      param->update_conn_params.conn_int,
                      param->update_conn_params.conn_int * 1.25,
                      param->update_conn_params.latency,
                      param->update_conn_params.timeout * 10);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            SOLAR_OS_LOGI(TAG, "BLE Authentication SUCCESS with peer [" ESP_BD_ADDR_STR "]",
                          ESP_BD_ADDR_HEX(param->ble_security.auth_cmpl.bd_addr));
        } else {
            SOLAR_OS_LOGW(TAG, "BLE Authentication FAILED with peer [" ESP_BD_ADDR_STR "] reason=0x%x",
                          ESP_BD_ADDR_HEX(param->ble_security.auth_cmpl.bd_addr),
                          param->ble_security.auth_cmpl.fail_reason);
        }
        break;

    default:
        break;
    }

    /* Dispatch to registered module callbacks */
    for (size_t i = 0; i < SOLAR_OS_BLE_MAX_GAP_CALLBACKS; i++) {
        if (s_gap_callbacks[i] != NULL) {
            s_gap_callbacks[i](event, param);
        }
    }
}

esp_err_t solar_os_ble_scan_start(uint32_t duration_sec,
                                  solar_os_ble_scan_item_t *results,
                                  size_t max_results,
                                  size_t *out_count)
{
    if (results == NULL || max_results == 0 || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    esp_err_t ret = solar_os_ble_core_init();
    if (ret != ESP_OK) return ret;

    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    while (xSemaphoreTake(s_scan_done_sem, 0) == pdTRUE) {}

    s_active_scan_results = results;
    s_active_scan_max = max_results;
    s_active_scan_count = 0;
    memset(results, 0, max_results * sizeof(solar_os_ble_scan_item_t));

    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
    };

    ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        s_active_scan_results = NULL;
        s_active_scan_max = 0;
        s_active_scan_count = 0;
        xSemaphoreGive(s_scan_mutex);
        return ret;
    }

    (void)xSemaphoreTake(s_scan_done_sem, pdMS_TO_TICKS(1000));

    s_is_scanning = true;
    const uint32_t scan_sec = duration_sec > 0 ? duration_sec : 3U;
    ret = esp_ble_gap_start_scanning(scan_sec);
    if (ret != ESP_OK) {
        s_is_scanning = false;
        s_active_scan_results = NULL;
        s_active_scan_max = 0;
        s_active_scan_count = 0;
        xSemaphoreGive(s_scan_mutex);
        return ret;
    }

    const TickType_t wait_ticks = pdMS_TO_TICKS((scan_sec + 2U) * 1000U);
    (void)xSemaphoreTake(s_scan_done_sem, wait_ticks);

    if (s_is_scanning) {
        (void)esp_ble_gap_stop_scanning();
        s_is_scanning = false;
    }

    *out_count = s_active_scan_count;
    s_active_scan_results = NULL;
    s_active_scan_max = 0;
    s_active_scan_count = 0;

    xSemaphoreGive(s_scan_mutex);
    return ESP_OK;
}

esp_err_t solar_os_ble_scan_stop(void)
{
    if (!s_is_scanning) return ESP_OK;
    esp_err_t ret = esp_ble_gap_stop_scanning();
    s_is_scanning = false;
    if (s_scan_done_sem != NULL) {
        xSemaphoreGive(s_scan_done_sem);
    }
    return ret;
}

bool solar_os_ble_is_scanning(void)
{
    return s_is_scanning;
}
