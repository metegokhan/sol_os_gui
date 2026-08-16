#ifndef SOLAR_OS_BLE_KEYBOARD_H
#define SOLAR_OS_BLE_KEYBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_keys.h"
#include "solar_os_ble_core.h"
#include "solar_os_ble_hid.h"
#include "solar_os_ble_gatt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_KEYBOARD_MAX_KEYS 6
#define SOLAR_OS_BLE_KEYBOARD_SCAN_MAX_RESULTS 32
#define SOLAR_OS_BLE_KEYBOARD_MAX_REMEMBERED SOLAR_OS_BLE_HID_MAX_CONNECTED

typedef enum {
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_US = 0,
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_TR = 1,
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE = 2,
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_COUNT = 3,
} solar_os_ble_keyboard_layout_t;

typedef enum {
    BLE_KEYBOARD_IDLE = 0,
    BLE_KEYBOARD_SCANNING,
    BLE_KEYBOARD_CONNECTING,
    BLE_KEYBOARD_PASSKEY,
    BLE_KEYBOARD_CONNECTED,
    BLE_KEYBOARD_FAILED,
} solar_os_ble_keyboard_status_t;

typedef struct {
    uint8_t modifiers;
    uint8_t keycodes[BLE_KEYBOARD_MAX_KEYS];
    char chars[BLE_KEYBOARD_MAX_KEYS];
} solar_os_ble_keyboard_key_state_t;

/* Backward compatible scan result typedef */
typedef solar_os_ble_scan_item_t solar_os_ble_keyboard_scan_result_t;

/**
 * @brief Initialize Keyboard subsystem and input source.
 */
esp_err_t solar_os_ble_keyboard_init(void);

/**
 * @brief Process an incoming HID keyboard report.
 */
void solar_os_ble_keyboard_process_report(uint8_t map_index,
                                          uint16_t report_id,
                                          const uint8_t *data,
                                          uint16_t length);

/**
 * @brief Process an incoming HID consumer control report (volume, play/pause, media keys).
 */
void solar_os_ble_keyboard_process_consumer_report(uint16_t report_id,
                                                   const uint8_t *data,
                                                   uint16_t length);

/**
 * @brief Reset internal key state tracking.
 */
void solar_os_ble_keyboard_reset_state(bool connected);

/**
 * @brief Layout configuration APIs.
 */
solar_os_ble_keyboard_layout_t solar_os_ble_keyboard_get_layout(void);
solar_os_ble_keyboard_layout_t solar_os_ble_keyboard_layout(void);
esp_err_t solar_os_ble_keyboard_set_layout(solar_os_ble_keyboard_layout_t layout);
const char *solar_os_ble_keyboard_layout_name(solar_os_ble_keyboard_layout_t layout);
bool solar_os_ble_keyboard_parse_layout(const char *name, solar_os_ble_keyboard_layout_t *layout);

/**
 * @brief Legacy compatibility wrappers.
 */
bool solar_os_ble_keyboard_is_connected(void);
solar_os_ble_keyboard_status_t solar_os_ble_keyboard_get_state(void);
void solar_os_ble_keyboard_get_status(char *buffer, size_t max_len);
void solar_os_ble_keyboard_get_status_text(char *buffer, size_t max_len);
void solar_os_ble_keyboard_get_key_state(solar_os_ble_keyboard_key_state_t *out_state);
size_t solar_os_ble_keyboard_read_chars(char *buffer, size_t max_len);
esp_err_t solar_os_ble_keyboard_forget(void);
esp_err_t solar_os_ble_keyboard_start_pairing(void);
size_t solar_os_ble_keyboard_remembered_count(void);
bool solar_os_ble_keyboard_is_scanning(void);

esp_err_t solar_os_ble_keyboard_scan(solar_os_ble_keyboard_scan_result_t *results,
                                     size_t max_results,
                                     size_t *found);

bool solar_os_ble_keyboard_enabled_for_current_boot(void);
bool solar_os_ble_keyboard_enabled_for_next_boot(void);
esp_err_t solar_os_ble_keyboard_set_enabled_for_next_boot(bool enabled);
esp_err_t solar_os_ble_keyboard_apply_boot_policy(void);
esp_err_t solar_os_ble_keyboard_prepare_sleep(uint32_t timeout_ms);
esp_err_t solar_os_ble_keyboard_resume(void);
const char *solar_os_ble_keyboard_addr_type_name(uint8_t addr_type);
bool solar_os_ble_keyboard_parse_addr_type(const char *name, uint8_t *addr_type);

#ifdef __cplusplus
}
#endif

#endif /* SOLAR_OS_BLE_KEYBOARD_H */
