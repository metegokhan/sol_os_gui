#ifndef SOLAR_OS_BLE_HID_H
#define SOLAR_OS_BLE_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_BLE_HID_MAX_CONNECTED 4

typedef enum {
    SOLAR_OS_BLE_DEV_TYPE_UNKNOWN = 0,
    SOLAR_OS_BLE_DEV_TYPE_KEYBOARD = 1,
    SOLAR_OS_BLE_DEV_TYPE_MOUSE = 2,
    SOLAR_OS_BLE_DEV_TYPE_GAMEPAD = 3,
    SOLAR_OS_BLE_DEV_TYPE_CUSTOM = 4,
} solar_os_ble_dev_type_t;

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
    char name[64];
    solar_os_ble_dev_type_t type;
    bool connected;
    uint8_t battery_level;
} solar_os_ble_connected_dev_info_t;

/**
 * @brief Initialize BLE HID Host subsystem.
 */
esp_err_t solar_os_ble_hid_init(void);

/**
 * @brief Get total number of currently connected HID devices.
 */
size_t solar_os_ble_hid_connected_count(void);

/**
 * @brief Retrieve information for a connected HID device by index.
 */
bool solar_os_ble_hid_get_connected_dev(size_t index, solar_os_ble_connected_dev_info_t *out);

/**
 * @brief Check if a device with given MAC is currently connected.
 */
bool solar_os_ble_hid_is_connected(const uint8_t bda[6]);

/**
 * @brief Connect/Pair to a BLE HID device (Keyboard, Mouse, Gamepad).
 */
esp_err_t solar_os_ble_hid_connect(const uint8_t bda[6], uint8_t addr_type, const char *name);

/**
 * @brief Disconnect a connected BLE HID device.
 */
esp_err_t solar_os_ble_hid_disconnect(const uint8_t bda[6]);

/**
 * @brief Get current HID host summary status string.
 */
void solar_os_ble_hid_get_status(char *out_status, size_t max_len);

/**
 * @brief Check if any keyboard is currently connected.
 */
bool solar_os_ble_hid_has_keyboard(void);

/**
 * @brief Get total number of remembered HID devices in NVS.
 */
size_t solar_os_ble_hid_remembered_count(void);

/**
 * @brief Forget all remembered HID devices from NVS.
 */
esp_err_t solar_os_ble_hid_forget_all(void);

/**
 * @brief Start background auto-discovery & pairing task for keyboards/HID devices.
 */
esp_err_t solar_os_ble_hid_start_pairing(void);

#ifdef __cplusplus
}
#endif

#endif /* SOLAR_OS_BLE_HID_H */
