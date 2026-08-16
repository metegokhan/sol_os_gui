#ifndef SOLAR_OS_BLE_GATT_H
#define SOLAR_OS_BLE_GATT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_BLE_GATT_MAX_SERVICES 24
#define SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS 48
#define SOLAR_OS_BLE_GATT_VALUE_MAX 256
#define SOLAR_OS_BLE_GATT_STATUS_MAX 64

typedef struct {
    char uuid[40];
    uint16_t start_handle;
    uint16_t end_handle;
    bool primary;
} solar_os_ble_gatt_service_t;

typedef struct {
    char uuid[40];
    uint16_t handle;
    uint8_t properties;
} solar_os_ble_gatt_characteristic_t;

typedef struct {
    bool connected;
    uint8_t bda[6];
    uint8_t addr_type;
    uint16_t conn_id;
    uint16_t mtu;
    size_t service_count;
    char status[SOLAR_OS_BLE_GATT_STATUS_MAX];
} solar_os_ble_gatt_status_t;

/**
 * @brief Initialize GATT client subsystem.
 */
esp_err_t solar_os_ble_gatt_init(void);

/**
 * @brief Connect to peripheral for GATT attribute exploration.
 */
esp_err_t solar_os_ble_gatt_connect(const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms);

/**
 * @brief Disconnect active GATT client connection.
 */
esp_err_t solar_os_ble_gatt_disconnect(void);

/**
 * @brief Retrieve current GATT client status.
 */
void solar_os_ble_gatt_get_status(solar_os_ble_gatt_status_t *status);

/**
 * @brief Fetch discovered services.
 */
esp_err_t solar_os_ble_gatt_services(solar_os_ble_gatt_service_t *services,
                                     size_t max_services,
                                     size_t *count);

/**
 * @brief Fetch characteristics for a discovered service.
 */
esp_err_t solar_os_ble_gatt_characteristics(size_t service_index,
                                            solar_os_ble_gatt_characteristic_t *characteristics,
                                            size_t max_characteristics,
                                            size_t *count);

/**
 * @brief Read characteristic value by handle.
 */
esp_err_t solar_os_ble_gatt_read(uint16_t handle,
                                 uint8_t *value,
                                 size_t max_len,
                                 size_t *value_len,
                                 uint32_t timeout_ms);

/**
 * @brief Write characteristic value by handle.
 */
esp_err_t solar_os_ble_gatt_write(uint16_t handle,
                                  const uint8_t *value,
                                  size_t len,
                                  bool response,
                                  uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* SOLAR_OS_BLE_GATT_H */
