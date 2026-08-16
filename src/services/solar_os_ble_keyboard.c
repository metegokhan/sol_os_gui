/*
 * Solar OS - Modular BLE Keyboard Driver
 * Handles US/TR/DE keyboard layout translation, repeat state, consumer keys, and Solar OS input injection.
 */

#include "solar_os_ble_keyboard.h"
#include "solar_os_ble_core.h"
#include "solar_os_ble_hid.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "solar_os_hid_keyboard_report.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"

#define TAG "ble_kb"
#define BLE_KEYBOARD_NVS_NAMESPACE "ble_kb"
#define BLE_KEYBOARD_NVS_LAYOUT_KEY "layout"

#define HID_MOD_CTRL 0x11
#define HID_MOD_SHIFT 0x22
#define HID_MOD_ALT 0x44

static const char *const keyboard_layout_names[] = {
    [SOLAR_OS_BLE_KEYBOARD_LAYOUT_US] = "us",
    [SOLAR_OS_BLE_KEYBOARD_LAYOUT_TR] = "tr",
    [SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE] = "de",
};

static bool s_kb_initialized = false;
static solar_os_input_source_t s_input_source = SOLAR_OS_INPUT_SOURCE_INVALID;
static solar_os_ble_keyboard_layout_t s_keyboard_layout = SOLAR_OS_BLE_KEYBOARD_LAYOUT_US;
static bool s_caps_lock = false;
static uint8_t s_previous_keys[BLE_KEYBOARD_MAX_KEYS] = {0};
static uint8_t s_previous_modifiers = 0;
static uint16_t s_previous_consumer_usage = 0;
static uint8_t s_previous_consumer_key = 0;
static solar_os_hid_keyboard_report_tracker_t s_tracker;
static solar_os_ble_keyboard_key_state_t s_key_state = {0};
static portMUX_TYPE s_key_state_lock = portMUX_INITIALIZER_UNLOCKED;

/* Forward declarations */
static char hid_keycode_to_char_us(uint8_t keycode, bool shift);
static char hid_keycode_to_char_tr(uint8_t keycode, uint8_t modifiers);
static char hid_keycode_to_char_de(uint8_t keycode, uint8_t modifiers);
static char hid_keycode_to_function_key(uint8_t keycode);
static char hid_keycode_to_nav_key(uint8_t keycode, uint8_t modifiers);
static char hid_keycode_to_control_char(uint8_t keycode);
static char hid_keycode_to_system_key(uint8_t keycode, uint8_t modifiers);

static esp_err_t load_keyboard_layout(solar_os_ble_keyboard_layout_t *out_layout)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) return ret;

    uint8_t raw_layout = (uint8_t)SOLAR_OS_BLE_KEYBOARD_LAYOUT_US;
    ret = nvs_get_u8(nvs, BLE_KEYBOARD_NVS_LAYOUT_KEY, &raw_layout);
    nvs_close(nvs);
    if (ret == ESP_OK && raw_layout < SOLAR_OS_BLE_KEYBOARD_LAYOUT_COUNT) {
        *out_layout = (solar_os_ble_keyboard_layout_t)raw_layout;
    }
    return ret;
}

static esp_err_t save_keyboard_layout(solar_os_ble_keyboard_layout_t layout)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_u8(nvs, BLE_KEYBOARD_NVS_LAYOUT_KEY, (uint8_t)layout);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

solar_os_ble_keyboard_layout_t solar_os_ble_keyboard_get_layout(void)
{
    return s_keyboard_layout;
}

solar_os_ble_keyboard_layout_t solar_os_ble_keyboard_layout(void)
{
    return s_keyboard_layout;
}

esp_err_t solar_os_ble_keyboard_set_layout(solar_os_ble_keyboard_layout_t layout)
{
    if (layout >= SOLAR_OS_BLE_KEYBOARD_LAYOUT_COUNT) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = solar_os_input_set_keyboard_layout((solar_os_input_keyboard_layout_t)layout);
    if (ret != ESP_OK) return ret;
    s_keyboard_layout = layout;
    return save_keyboard_layout(layout);
}

const char *solar_os_ble_keyboard_layout_name(solar_os_ble_keyboard_layout_t layout)
{
    if ((size_t)layout >= sizeof(keyboard_layout_names) / sizeof(keyboard_layout_names[0])) {
        return "unknown";
    }
    return keyboard_layout_names[layout];
}

bool solar_os_ble_keyboard_parse_layout(const char *name, solar_os_ble_keyboard_layout_t *layout)
{
    if (name == NULL || layout == NULL) return false;
    for (size_t i = 0; i < sizeof(keyboard_layout_names) / sizeof(keyboard_layout_names[0]); i++) {
        if (strcmp(name, keyboard_layout_names[i]) == 0) {
            *layout = (solar_os_ble_keyboard_layout_t)i;
            return true;
        }
    }
    return false;
}

esp_err_t solar_os_ble_keyboard_init(void)
{
    if (s_kb_initialized) return ESP_OK;

    if (s_input_source == SOLAR_OS_INPUT_SOURCE_INVALID) {
        ESP_RETURN_ON_ERROR(solar_os_input_source_open("ble-keyboard", &s_input_source),
                            TAG, "input source setup failed");
    }

    solar_os_hid_keyboard_report_reset(&s_tracker);

    solar_os_ble_keyboard_layout_t stored_layout = SOLAR_OS_BLE_KEYBOARD_LAYOUT_US;
    if (load_keyboard_layout(&stored_layout) == ESP_OK) {
        s_keyboard_layout = stored_layout;
    }
    (void)solar_os_input_set_keyboard_layout((solar_os_input_keyboard_layout_t)s_keyboard_layout);

    s_kb_initialized = true;
    return ESP_OK;
}

void solar_os_ble_keyboard_reset_state(bool connected)
{
    (void)connected;
    solar_os_hid_keyboard_report_reset(&s_tracker);
    memset(s_previous_keys, 0, sizeof(s_previous_keys));
    s_previous_modifiers = 0;
    s_previous_consumer_usage = 0;
    s_previous_consumer_key = 0;

    portENTER_CRITICAL(&s_key_state_lock);
    memset(&s_key_state, 0, sizeof(s_key_state));
    portEXIT_CRITICAL(&s_key_state_lock);
}

static bool key_in_report(uint8_t key, const uint8_t *keys)
{
    for (size_t i = 0; i < BLE_KEYBOARD_MAX_KEYS; i++) {
        if (keys[i] == key) return true;
    }
    return false;
}

void solar_os_ble_keyboard_process_report(uint8_t map_index,
                                          uint16_t report_id,
                                          const uint8_t *data,
                                          uint16_t length)
{
    if (!s_kb_initialized) return;

    solar_os_hid_keyboard_report_state_t report_state;
    if (!solar_os_hid_keyboard_report_update(&s_tracker, map_index, report_id, data, length, &report_state)) {
        return;
    }

    const uint8_t modifiers = report_state.modifiers;
    const uint8_t *keys = report_state.keys;

    if (key_in_report(0x01, keys)) {
        solar_os_ble_keyboard_reset_state(true);
        s_previous_modifiers = modifiers;
        return;
    }

    /* Key Releases */
    for (size_t i = 0; i < BLE_KEYBOARD_MAX_KEYS; i++) {
        const uint8_t key = s_previous_keys[i];
        if (key == 0 || key_in_report(key, keys)) continue;

        (void)solar_os_input_write_key(s_input_source,
                                       key,
                                       key,
                                       solar_os_input_translate_hid_usage(key, s_previous_modifiers, s_caps_lock),
                                       modifiers,
                                       SOLAR_OS_INPUT_KEY_RELEASE);
    }

    /* Key Presses */
    for (size_t i = 0; i < BLE_KEYBOARD_MAX_KEYS; i++) {
        const uint8_t key = keys[i];
        if (key == 0 || key_in_report(key, s_previous_keys)) continue;

        if (key == 0x39) {
            s_caps_lock = !s_caps_lock;
        }

        const uint8_t ch = solar_os_input_translate_hid_usage(key, modifiers, s_caps_lock);
        (void)solar_os_input_write_key(s_input_source,
                                       key,
                                       key,
                                       ch,
                                       modifiers,
                                       SOLAR_OS_INPUT_KEY_PRESS);
    }

    /* Publish state */
    portENTER_CRITICAL(&s_key_state_lock);
    s_key_state.modifiers = modifiers;
    for (size_t i = 0; i < BLE_KEYBOARD_MAX_KEYS; i++) {
        s_key_state.keycodes[i] = keys[i];
        s_key_state.chars[i] = solar_os_input_translate_hid_usage(keys[i], modifiers, s_caps_lock);
    }
    portEXIT_CRITICAL(&s_key_state_lock);

    memcpy(s_previous_keys, keys, sizeof(s_previous_keys));
    s_previous_modifiers = modifiers;
}

void solar_os_ble_keyboard_process_consumer_report(uint16_t report_id,
                                                   const uint8_t *data,
                                                   uint16_t length)
{
    if (data == NULL || length == 0 || !s_kb_initialized) return;

    if (length > 1 && (data[0] == (uint8_t)report_id || data[0] <= 8)) {
        data++;
        length--;
    }

    bool all_zero = true;
    for (uint16_t i = 0; i < length; i++) {
        if (data[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        if (s_previous_consumer_usage != 0) {
            (void)solar_os_input_write_key(s_input_source,
                                           s_previous_consumer_usage,
                                           s_previous_consumer_usage,
                                           s_previous_consumer_key,
                                           0,
                                           SOLAR_OS_INPUT_KEY_RELEASE);
            s_previous_consumer_usage = 0;
            s_previous_consumer_key = 0;
        }
        return;
    }

    uint8_t key = 0;
    uint16_t usage = 0;

    for (uint16_t i = 0; i + 1 < length; i += 2) {
        uint16_t u16 = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
        if (u16 == 0) continue;
        usage = u16;
        switch (u16) {
        case 0x0001:
        case 0x00E9: key = SOLAR_OS_KEY_AUDIO_VOLUME_UP; usage = 0x00E9; break;
        case 0x8000:
        case 0x00EA: key = SOLAR_OS_KEY_AUDIO_VOLUME_DOWN; usage = 0x00EA; break;
        case 0x4000:
        case 0x00E2: key = SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE; usage = 0x00E2; break;
        case 0x00CD: key = ' '; break;
        case 0x00B5: key = SOLAR_OS_KEY_RIGHT; break;
        case 0x00B6: key = SOLAR_OS_KEY_LEFT; break;
        case 0x00B7: key = SOLAR_OS_KEY_ESCAPE; break;
        case 0x0221: key = 's'; break;
        case 0x0223: key = 'h'; break;
        case 0x0224: key = SOLAR_OS_KEY_LEFT; break;
        case 0x0225: key = SOLAR_OS_KEY_RIGHT; break;
        default:
            key = solar_os_input_translate_hid_usage(u16, 0, false);
            break;
        }
        if (usage != 0) break;
    }

    if (usage != 0) {
        if (s_previous_consumer_usage == usage) {
            (void)solar_os_input_write_key(s_input_source, usage, usage, key, 0, SOLAR_OS_INPUT_KEY_REPEAT);
        } else {
            if (s_previous_consumer_usage != 0) {
                (void)solar_os_input_write_key(s_input_source, s_previous_consumer_usage, s_previous_consumer_usage, s_previous_consumer_key, 0, SOLAR_OS_INPUT_KEY_RELEASE);
            }
            (void)solar_os_input_write_key(s_input_source, usage, usage, key, 0, SOLAR_OS_INPUT_KEY_PRESS);
            if (key >= 32 && key <= 126) {
                (void)solar_os_input_write_char(s_input_source, (char)key);
            }
            s_previous_consumer_usage = usage;
            s_previous_consumer_key = key;
        }
    }
}

/* ---------------------------------------------------------------------
 * Compatibility Wrappers
 * ------------------------------------------------------------------- */

bool solar_os_ble_keyboard_is_connected(void)
{
    return solar_os_ble_hid_has_keyboard();
}

solar_os_ble_keyboard_status_t solar_os_ble_keyboard_get_state(void)
{
    return solar_os_ble_hid_has_keyboard() ? BLE_KEYBOARD_CONNECTED : BLE_KEYBOARD_IDLE;
}

void solar_os_ble_keyboard_get_status(char *buffer, size_t max_len)
{
    if (buffer == NULL || max_len == 0) return;
    solar_os_ble_hid_get_status(buffer, max_len);
}

void solar_os_ble_keyboard_get_status_text(char *buffer, size_t max_len)
{
    if (buffer == NULL || max_len == 0) return;
    solar_os_ble_hid_get_status(buffer, max_len);
}

void solar_os_ble_keyboard_get_key_state(solar_os_ble_keyboard_key_state_t *out_state)
{
    if (out_state == NULL) return;
    portENTER_CRITICAL(&s_key_state_lock);
    *out_state = s_key_state;
    portEXIT_CRITICAL(&s_key_state_lock);
}

size_t solar_os_ble_keyboard_read_chars(char *buffer, size_t max_len)
{
    if (buffer == NULL || max_len == 0) return 0;
    size_t count = 0;
    portENTER_CRITICAL(&s_key_state_lock);
    for (size_t i = 0; i < BLE_KEYBOARD_MAX_KEYS && count < max_len; i++) {
        if (s_key_state.chars[i] != '\0') {
            buffer[count++] = s_key_state.chars[i];
        }
    }
    portEXIT_CRITICAL(&s_key_state_lock);
    return count;
}

esp_err_t solar_os_ble_keyboard_forget(void)
{
    return solar_os_ble_hid_forget_all();
}

esp_err_t solar_os_ble_keyboard_start_pairing(void)
{
    return solar_os_ble_hid_start_pairing();
}

size_t solar_os_ble_keyboard_remembered_count(void)
{
    return solar_os_ble_hid_remembered_count();
}

bool solar_os_ble_keyboard_is_scanning(void)
{
    return solar_os_ble_is_scanning();
}

esp_err_t solar_os_ble_keyboard_scan(solar_os_ble_keyboard_scan_result_t *results,
                                     size_t max_results,
                                     size_t *found)
{
    return solar_os_ble_scan_start(3, results, max_results, found);
}

bool solar_os_ble_keyboard_enabled_for_current_boot(void)
{
    return solar_os_ble_core_enabled_for_current_boot();
}

bool solar_os_ble_keyboard_enabled_for_next_boot(void)
{
    return solar_os_ble_core_enabled_for_next_boot();
}

esp_err_t solar_os_ble_keyboard_set_enabled_for_next_boot(bool enabled)
{
    return solar_os_ble_core_set_enabled_for_next_boot(enabled);
}

esp_err_t solar_os_ble_keyboard_apply_boot_policy(void)
{
    return solar_os_ble_core_apply_boot_policy();
}

esp_err_t solar_os_ble_keyboard_prepare_sleep(uint32_t timeout_ms)
{
    (void)timeout_ms;
    (void)solar_os_ble_scan_stop();
    return ESP_OK;
}

esp_err_t solar_os_ble_keyboard_resume(void)
{
    return ESP_OK;
}

const char *solar_os_ble_keyboard_addr_type_name(uint8_t addr_type)
{
    return addr_type == 0 ? "public" : "random";
}

bool solar_os_ble_keyboard_parse_addr_type(const char *name, uint8_t *addr_type)
{
    if (name == NULL || addr_type == NULL) return false;
    if (strcasecmp(name, "public") == 0) {
        *addr_type = 0;
        return true;
    }
    if (strcasecmp(name, "random") == 0) {
        *addr_type = 1;
        return true;
    }
    return false;
}
