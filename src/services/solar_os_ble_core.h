#ifndef SOLAR_OS_BLE_CORE_H
#define SOLAR_OS_BLE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_BLE_NAME_MAX 64
#define SOLAR_OS_BLE_KEYBOARD_NAME_MAX SOLAR_OS_BLE_NAME_MAX
#define SOLAR_OS_BLE_ADV_DATA_MAX 64
#define SOLAR_OS_BLE_MAX_GAP_CALLBACKS 4
#define SOLAR_OS_BLE_MAX_GATTC_CALLBACKS 4

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
    char name[SOLAR_OS_BLE_NAME_MAX];
    uint8_t adv_data[SOLAR_OS_BLE_ADV_DATA_MAX];
    uint8_t adv_data_len;
} solar_os_ble_scan_item_t;

typedef void (*solar_os_ble_gap_event_cb_t)(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
typedef void (*solar_os_ble_gattc_event_cb_t)(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);

/**
 * @brief Initialize BLE controller and Bluedroid stack safely (idempotent).
 */
esp_err_t solar_os_ble_core_init(void);

/**
 * @brief Returns true if BLE stack is initialized.
 */
bool solar_os_ble_core_is_initialized(void);

/**
 * @brief Boot policy APIs.
 */
bool solar_os_ble_core_enabled_for_current_boot(void);
bool solar_os_ble_core_enabled_for_next_boot(void);
esp_err_t solar_os_ble_core_set_enabled_for_next_boot(bool enabled);
esp_err_t solar_os_ble_core_apply_boot_policy(void);

/**
 * @brief Register/Unregister GAP event listeners (e.g. for HID Host or custom modules).
 */
esp_err_t solar_os_ble_register_gap_callback(solar_os_ble_gap_event_cb_t cb);
esp_err_t solar_os_ble_unregister_gap_callback(solar_os_ble_gap_event_cb_t cb);

/**
 * @brief Register/Unregister GATTC event listeners.
 */
esp_err_t solar_os_ble_register_gattc_callback(solar_os_ble_gattc_event_cb_t cb);
esp_err_t solar_os_ble_unregister_gattc_callback(solar_os_ble_gattc_event_cb_t cb);

/**
 * @brief Thread-safe synchronous BLE GAP scanning.
 *        Guarantees that only one scan operates at a time.
 */
esp_err_t solar_os_ble_scan_start(uint32_t duration_sec,
                                  solar_os_ble_scan_item_t *results,
                                  size_t max_results,
                                  size_t *out_count);

/**
 * @brief Stop active BLE scan if running.
 */
esp_err_t solar_os_ble_scan_stop(void);

/**
 * @brief Check if scanning is actively in progress.
 */
bool solar_os_ble_is_scanning(void);

/**
 * @brief Advertising payload parsing helpers.
 */
uint16_t solar_os_ble_parse_appearance(const uint8_t *adv_data, uint16_t adv_len);
bool solar_os_ble_parse_has_uuid16(const uint8_t *adv_data, uint16_t adv_len, uint8_t ad_type, uint16_t target_uuid);
void solar_os_ble_parse_name(const uint8_t *adv_data, uint16_t adv_len, char *name_out, size_t max_len);

bool solar_os_ble_is_keyboard_like(uint16_t appearance, const char *name);
bool solar_os_ble_is_mouse_like(uint16_t appearance, const char *name);
bool solar_os_ble_is_gamepad_like(uint16_t appearance, const char *name);
bool solar_os_ble_is_hid_like(uint16_t appearance, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SOLAR_OS_BLE_CORE_H */
