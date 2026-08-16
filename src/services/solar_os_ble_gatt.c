/*
 * Solar OS - Modular Generic BLE GATT Client Service
 */

#include "solar_os_ble_gatt.h"
#include "solar_os_ble_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "solar_os_log.h"

#define TAG "ble_gatt"
#define BLE_GATT_APP_ID 1
#define BLE_GATT_INVALID_CONN_ID 0xFFFFU
#define BLE_GATT_CONNECT_TIMEOUT_MS 8000U
#define BLE_GATT_OPERATION_TIMEOUT_MS 3000U

typedef enum {
    BLE_GATT_OP_NONE = 0,
    BLE_GATT_OP_CONNECT,
    BLE_GATT_OP_READ,
    BLE_GATT_OP_WRITE,
} ble_gatt_operation_t;

typedef struct {
    bool registered;
    bool connecting;
    bool connected;
    esp_gatt_if_t gattc_if;
    uint16_t conn_id;
    uint8_t bda[6];
    esp_ble_addr_type_t addr_type;
    uint16_t mtu;
    solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
    size_t service_count;
    char status[SOLAR_OS_BLE_GATT_STATUS_MAX];
    ble_gatt_operation_t op;
    esp_gatt_status_t op_status;
    uint8_t op_value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t op_value_len;
} ble_gatt_runtime_state_t;

static ble_gatt_runtime_state_t s_gatt_state = {
    .gattc_if = ESP_GATT_IF_NONE,
    .conn_id = BLE_GATT_INVALID_CONN_ID,
    .status = "idle",
};

static SemaphoreHandle_t s_gatt_mutex = NULL;
static SemaphoreHandle_t s_gatt_op_sem = NULL;
static bool s_gatt_initialized = false;

static void gatt_lock(void)
{
    if (s_gatt_mutex != NULL) {
        xSemaphoreTake(s_gatt_mutex, portMAX_DELAY);
    }
}

static void gatt_unlock(void)
{
    if (s_gatt_mutex != NULL) {
        xSemaphoreGive(s_gatt_mutex);
    }
}

static void gatt_set_status_locked(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_gatt_state.status, sizeof(s_gatt_state.status), fmt, args);
    va_end(args);
}

static void gatt_uuid_to_string(const esp_bt_uuid_t *uuid, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) return;
    if (uuid == NULL) {
        strlcpy(buffer, "-", buffer_len);
        return;
    }

    switch (uuid->len) {
    case ESP_UUID_LEN_16:
        snprintf(buffer, buffer_len, "0x%04x", (unsigned)uuid->uuid.uuid16);
        break;
    case ESP_UUID_LEN_32:
        snprintf(buffer, buffer_len, "0x%08" PRIx32, uuid->uuid.uuid32);
        break;
    case ESP_UUID_LEN_128:
        snprintf(buffer, buffer_len,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid->uuid.uuid128[15], uuid->uuid.uuid128[14], uuid->uuid.uuid128[13], uuid->uuid.uuid128[12],
                 uuid->uuid.uuid128[11], uuid->uuid.uuid128[10], uuid->uuid.uuid128[9], uuid->uuid.uuid128[8],
                 uuid->uuid.uuid128[7], uuid->uuid.uuid128[6], uuid->uuid.uuid128[5], uuid->uuid.uuid128[4],
                 uuid->uuid.uuid128[3], uuid->uuid.uuid128[2], uuid->uuid.uuid128[1], uuid->uuid.uuid128[0]);
        break;
    default:
        snprintf(buffer, buffer_len, "uuid-%u", (unsigned)uuid->len);
        break;
    }
}

static void gatt_drain_op_sem(void)
{
    if (s_gatt_op_sem == NULL) return;
    while (xSemaphoreTake(s_gatt_op_sem, 0) == pdTRUE) {}
}

static void gatt_complete_operation(ble_gatt_operation_t op, esp_gatt_status_t status)
{
    bool should_signal = false;
    gatt_lock();
    if (s_gatt_state.op == op || op == BLE_GATT_OP_NONE) {
        s_gatt_state.op_status = status;
        s_gatt_state.op = BLE_GATT_OP_NONE;
        should_signal = true;
    }
    gatt_unlock();

    if (should_signal && s_gatt_op_sem != NULL) {
        xSemaphoreGive(s_gatt_op_sem);
    }
}

static esp_err_t gatt_begin_operation(ble_gatt_operation_t op)
{
    gatt_lock();
    if (s_gatt_state.op != BLE_GATT_OP_NONE) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_gatt_state.op = op;
    s_gatt_state.op_status = ESP_GATT_OK;
    s_gatt_state.op_value_len = 0;
    gatt_drain_op_sem();
    gatt_unlock();
    return ESP_OK;
}

static esp_err_t gatt_wait_operation(ble_gatt_operation_t op, uint32_t timeout_ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(timeout_ms != 0 ? timeout_ms : BLE_GATT_OPERATION_TIMEOUT_MS);
    if (s_gatt_op_sem == NULL || xSemaphoreTake(s_gatt_op_sem, ticks) != pdTRUE) {
        gatt_lock();
        if (s_gatt_state.op == op) {
            s_gatt_state.op = BLE_GATT_OP_NONE;
            gatt_set_status_locked("timeout");
        }
        gatt_unlock();
        return ESP_ERR_TIMEOUT;
    }

    gatt_lock();
    const esp_gatt_status_t status = s_gatt_state.op_status;
    gatt_unlock();

    return status == ESP_GATT_OK ? ESP_OK : ESP_FAIL;
}

static void solaros_gattc_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param)
{
    if (param == NULL) return;

    switch (event) {
    case ESP_GATTC_REG_EVT:
        gatt_lock();
        if (param->reg.status == ESP_GATT_OK && param->reg.app_id == BLE_GATT_APP_ID) {
            s_gatt_state.gattc_if = gattc_if;
            s_gatt_state.registered = true;
            gatt_set_status_locked("idle");
        }
        gatt_unlock();
        if (s_gatt_op_sem != NULL) {
            xSemaphoreGive(s_gatt_op_sem);
        }
        break;

    case ESP_GATTC_OPEN_EVT:
        if (gattc_if != s_gatt_state.gattc_if) return;
        gatt_lock();
        if (param->open.status != ESP_GATT_OK) {
            s_gatt_state.connecting = false;
            s_gatt_state.connected = false;
            s_gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
            gatt_set_status_locked("open failed 0x%02x", param->open.status);
            gatt_unlock();
            gatt_complete_operation(BLE_GATT_OP_CONNECT, param->open.status);
            break;
        }

        s_gatt_state.connected = true;
        s_gatt_state.connecting = false;
        s_gatt_state.conn_id = param->open.conn_id;
        s_gatt_state.mtu = param->open.mtu;
        memcpy(s_gatt_state.bda, param->open.remote_bda, 6);
        s_gatt_state.service_count = 0;
        gatt_set_status_locked("discovering");
        gatt_unlock();

        (void)esp_ble_gattc_send_mtu_req(gattc_if, param->open.conn_id);
        if (esp_ble_gattc_search_service(gattc_if, param->open.conn_id, NULL) != ESP_OK) {
            gatt_lock();
            gatt_set_status_locked("service discovery failed");
            gatt_unlock();
            gatt_complete_operation(BLE_GATT_OP_CONNECT, ESP_GATT_ERROR);
        }
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        gatt_lock();
        if (s_gatt_state.connected && s_gatt_state.conn_id == param->cfg_mtu.conn_id &&
            param->cfg_mtu.status == ESP_GATT_OK) {
            s_gatt_state.mtu = param->cfg_mtu.mtu;
        }
        gatt_unlock();
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        gatt_lock();
        if (s_gatt_state.connected && s_gatt_state.conn_id == param->search_res.conn_id &&
            s_gatt_state.service_count < SOLAR_OS_BLE_GATT_MAX_SERVICES) {
            solar_os_ble_gatt_service_t *srv = &s_gatt_state.services[s_gatt_state.service_count++];
            srv->start_handle = param->search_res.start_handle;
            srv->end_handle = param->search_res.end_handle;
            srv->primary = param->search_res.is_primary;
            gatt_uuid_to_string(&param->search_res.srvc_id.uuid, srv->uuid, sizeof(srv->uuid));
        }
        gatt_unlock();
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        gatt_lock();
        if (s_gatt_state.connected && s_gatt_state.conn_id == param->search_cmpl.conn_id) {
            if (param->search_cmpl.status == ESP_GATT_OK) {
                gatt_set_status_locked("connected");
            } else {
                gatt_set_status_locked("search failed 0x%02x", param->search_cmpl.status);
            }
        }
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_CONNECT, param->search_cmpl.status);
        break;

    case ESP_GATTC_READ_CHAR_EVT:
        gatt_lock();
        if (s_gatt_state.connected && s_gatt_state.conn_id == param->read.conn_id &&
            s_gatt_state.op == BLE_GATT_OP_READ) {
            s_gatt_state.op_status = param->read.status;
            s_gatt_state.op_value_len = 0;
            if (param->read.status == ESP_GATT_OK && param->read.value != NULL) {
                const size_t copy = param->read.value_len < SOLAR_OS_BLE_GATT_VALUE_MAX ?
                                    param->read.value_len : SOLAR_OS_BLE_GATT_VALUE_MAX;
                memcpy(s_gatt_state.op_value, param->read.value, copy);
                s_gatt_state.op_value_len = copy;
                gatt_set_status_locked("read handle 0x%04x", param->read.handle);
            } else {
                gatt_set_status_locked("read failed 0x%02x", param->read.status);
            }
        }
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_READ, param->read.status);
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        gatt_lock();
        if (s_gatt_state.connected && s_gatt_state.conn_id == param->write.conn_id &&
            s_gatt_state.op == BLE_GATT_OP_WRITE) {
            s_gatt_state.op_status = param->write.status;
            gatt_set_status_locked(param->write.status == ESP_GATT_OK ?
                                   "write ok 0x%04x" : "write failed 0x%02x",
                                   param->write.handle);
        }
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_WRITE, param->write.status);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
        gatt_lock();
        if (s_gatt_state.connected &&
            (s_gatt_state.conn_id == param->disconnect.conn_id ||
             (param->disconnect.remote_bda != NULL &&
              memcmp(s_gatt_state.bda, param->disconnect.remote_bda, 6) == 0))) {
            s_gatt_state.connected = false;
            s_gatt_state.connecting = false;
            s_gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
            s_gatt_state.service_count = 0;
            gatt_set_status_locked("disconnected");
        }
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_NONE, ESP_GATT_ERROR);
        break;

    default:
        break;
    }
}

esp_err_t solar_os_ble_gatt_init(void)
{
    if (s_gatt_initialized) return ESP_OK;

    esp_err_t ret = solar_os_ble_core_init();
    if (ret != ESP_OK) return ret;

    if (s_gatt_mutex == NULL) {
        s_gatt_mutex = xSemaphoreCreateMutex();
    }
    if (s_gatt_op_sem == NULL) {
        s_gatt_op_sem = xSemaphoreCreateBinary();
    }

    ret = solar_os_ble_register_gattc_callback(solaros_gattc_event_handler);
    if (ret != ESP_OK) return ret;

    ret = esp_ble_gattc_app_register(BLE_GATT_APP_ID);
    if (ret != ESP_OK) return ret;

    s_gatt_initialized = true;
    return ESP_OK;
}

esp_err_t solar_os_ble_gatt_connect(const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms)
{
    if (bda == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = solar_os_ble_gatt_init();
    if (ret != ESP_OK) return ret;

    (void)solar_os_ble_scan_stop();

    esp_gatt_if_t gattc_if = ESP_GATT_IF_NONE;
    gatt_lock();
    if (!s_gatt_state.registered || s_gatt_state.gattc_if == ESP_GATT_IF_NONE) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_gatt_state.connected) {
        gatt_unlock();
        (void)solar_os_ble_gatt_disconnect();
        gatt_lock();
    }

    s_gatt_state.connecting = true;
    s_gatt_state.connected = false;
    s_gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
    s_gatt_state.addr_type = (esp_ble_addr_type_t)addr_type;
    s_gatt_state.service_count = 0;
    memcpy(s_gatt_state.bda, bda, 6);
    gatt_set_status_locked("connecting");
    gattc_if = s_gatt_state.gattc_if;
    gatt_unlock();

    ret = gatt_begin_operation(BLE_GATT_OP_CONNECT);
    if (ret != ESP_OK) return ret;

    ret = esp_ble_gattc_open(gattc_if, (uint8_t *)bda, (esp_ble_addr_type_t)addr_type, true);
    if (ret != ESP_OK) {
        gatt_lock();
        s_gatt_state.connecting = false;
        gatt_set_status_locked("connect failed %s", esp_err_to_name(ret));
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_CONNECT, ESP_GATT_ERROR);
        return ret;
    }

    return gatt_wait_operation(BLE_GATT_OP_CONNECT, timeout_ms != 0 ? timeout_ms : BLE_GATT_CONNECT_TIMEOUT_MS);
}

esp_err_t solar_os_ble_gatt_disconnect(void)
{
    if (!s_gatt_initialized) return ESP_OK;

    uint16_t conn_id = BLE_GATT_INVALID_CONN_ID;
    esp_gatt_if_t gattc_if = ESP_GATT_IF_NONE;

    gatt_lock();
    if (!s_gatt_state.connected) {
        s_gatt_state.connecting = false;
        gatt_set_status_locked("idle");
        gatt_unlock();
        return ESP_OK;
    }
    conn_id = s_gatt_state.conn_id;
    gattc_if = s_gatt_state.gattc_if;
    gatt_unlock();

    esp_err_t ret = esp_ble_gattc_close(gattc_if, conn_id);
    if (ret != ESP_OK) return ret;

    gatt_lock();
    s_gatt_state.connected = false;
    s_gatt_state.connecting = false;
    s_gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
    s_gatt_state.service_count = 0;
    gatt_set_status_locked("disconnected");
    gatt_unlock();
    return ESP_OK;
}

void solar_os_ble_gatt_get_status(solar_os_ble_gatt_status_t *status)
{
    if (status == NULL) return;
    gatt_lock();
    status->connected = s_gatt_state.connected;
    status->addr_type = (uint8_t)s_gatt_state.addr_type;
    status->conn_id = s_gatt_state.conn_id;
    status->mtu = s_gatt_state.mtu;
    status->service_count = s_gatt_state.service_count;
    memcpy(status->bda, s_gatt_state.bda, 6);
    strlcpy(status->status, s_gatt_state.status, sizeof(status->status));
    gatt_unlock();
}

esp_err_t solar_os_ble_gatt_services(solar_os_ble_gatt_service_t *services,
                                     size_t max_services,
                                     size_t *count)
{
    if (count != NULL) *count = 0;
    if (max_services > 0 && services == NULL) return ESP_ERR_INVALID_ARG;

    gatt_lock();
    if (!s_gatt_state.connected) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const size_t copy = s_gatt_state.service_count < max_services ? s_gatt_state.service_count : max_services;
    if (copy > 0) {
        memcpy(services, s_gatt_state.services, copy * sizeof(services[0]));
    }
    if (count != NULL) {
        *count = s_gatt_state.service_count;
    }
    gatt_unlock();
    return ESP_OK;
}

esp_err_t solar_os_ble_gatt_characteristics(size_t service_index,
                                            solar_os_ble_gatt_characteristic_t *characteristics,
                                            size_t max_characteristics,
                                            size_t *count)
{
    if (count != NULL) *count = 0;
    if (max_characteristics > 0 && characteristics == NULL) return ESP_ERR_INVALID_ARG;

    solar_os_ble_gatt_service_t service = {0};
    uint16_t conn_id = BLE_GATT_INVALID_CONN_ID;
    esp_gatt_if_t gattc_if = ESP_GATT_IF_NONE;

    gatt_lock();
    if (!s_gatt_state.connected) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (service_index >= s_gatt_state.service_count) {
        gatt_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    service = s_gatt_state.services[service_index];
    conn_id = s_gatt_state.conn_id;
    gattc_if = s_gatt_state.gattc_if;
    gatt_unlock();

    esp_gattc_char_elem_t chars[SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS] = {0};
    uint16_t char_count = max_characteristics > SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS ?
                          SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS : (uint16_t)max_characteristics;
    if (char_count == 0) return ESP_OK;

    esp_gatt_status_t status = esp_ble_gattc_get_all_char(gattc_if,
                                                          conn_id,
                                                          service.start_handle,
                                                          service.end_handle,
                                                          chars,
                                                          &char_count,
                                                          0);
    if (status != ESP_GATT_OK) return ESP_FAIL;

    for (uint16_t i = 0; i < char_count; i++) {
        characteristics[i].handle = chars[i].char_handle;
        characteristics[i].properties = chars[i].properties;
        gatt_uuid_to_string(&chars[i].uuid, characteristics[i].uuid, sizeof(characteristics[i].uuid));
    }
    if (count != NULL) {
        *count = char_count;
    }
    return ESP_OK;
}

esp_err_t solar_os_ble_gatt_read(uint16_t handle,
                                 uint8_t *value,
                                 size_t max_len,
                                 size_t *value_len,
                                 uint32_t timeout_ms)
{
    if (value_len != NULL) *value_len = 0;
    if (handle == 0 || (max_len > 0 && value == NULL)) return ESP_ERR_INVALID_ARG;

    gatt_lock();
    const bool can_read = s_gatt_state.connected && s_gatt_state.gattc_if != ESP_GATT_IF_NONE;
    const uint16_t conn_id = s_gatt_state.conn_id;
    const esp_gatt_if_t gattc_if = s_gatt_state.gattc_if;
    gatt_unlock();
    if (!can_read) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = gatt_begin_operation(BLE_GATT_OP_READ);
    if (ret != ESP_OK) return ret;

    ret = esp_ble_gattc_read_char(gattc_if, conn_id, handle, ESP_GATT_AUTH_REQ_NONE);
    if (ret != ESP_OK) {
        gatt_complete_operation(BLE_GATT_OP_READ, ESP_GATT_ERROR);
        return ret;
    }

    ret = gatt_wait_operation(BLE_GATT_OP_READ, timeout_ms != 0 ? timeout_ms : BLE_GATT_OPERATION_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;

    gatt_lock();
    const size_t copy = s_gatt_state.op_value_len < max_len ? s_gatt_state.op_value_len : max_len;
    if (copy > 0) {
        memcpy(value, s_gatt_state.op_value, copy);
    }
    if (value_len != NULL) {
        *value_len = s_gatt_state.op_value_len;
    }
    gatt_unlock();
    return ESP_OK;
}

esp_err_t solar_os_ble_gatt_write(uint16_t handle,
                                  const uint8_t *value,
                                  size_t len,
                                  bool response,
                                  uint32_t timeout_ms)
{
    if (handle == 0 || (len > 0 && value == NULL)) return ESP_ERR_INVALID_ARG;

    gatt_lock();
    const bool can_write = s_gatt_state.connected && s_gatt_state.gattc_if != ESP_GATT_IF_NONE;
    const uint16_t conn_id = s_gatt_state.conn_id;
    const esp_gatt_if_t gattc_if = s_gatt_state.gattc_if;
    gatt_unlock();
    if (!can_write) return ESP_ERR_INVALID_STATE;

    const esp_gatt_write_type_t write_type = response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP;

    if (response) {
        esp_err_t ret = gatt_begin_operation(BLE_GATT_OP_WRITE);
        if (ret != ESP_OK) return ret;

        ret = esp_ble_gattc_write_char(gattc_if, conn_id, handle, len, (uint8_t *)value, write_type, ESP_GATT_AUTH_REQ_NONE);
        if (ret != ESP_OK) {
            gatt_complete_operation(BLE_GATT_OP_WRITE, ESP_GATT_ERROR);
            return ret;
        }

        return gatt_wait_operation(BLE_GATT_OP_WRITE, timeout_ms != 0 ? timeout_ms : BLE_GATT_OPERATION_TIMEOUT_MS);
    }

    return esp_ble_gattc_write_char(gattc_if, conn_id, handle, len, (uint8_t *)value, write_type, ESP_GATT_AUTH_REQ_NONE);
}
