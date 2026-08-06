#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"

static int64_t now_us;

int64_t esp_timer_get_time(void)
{
    return now_us;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t length = strlen(src);
    if (size > 0) {
        const size_t copy = length < size - 1U ? length : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return length;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    (void)name;
    if (mode == NVS_READONLY) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

static solar_os_input_key_event_t read_one_event(void)
{
    solar_os_input_key_event_t event = {0};
    assert(solar_os_input_read_events(&event, 1) == 1);
    return event;
}

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);

    solar_os_input_source_t keyboard = SOLAR_OS_INPUT_SOURCE_INVALID;
    solar_os_input_source_t buttons = SOLAR_OS_INPUT_SOURCE_INVALID;
    assert(solar_os_input_source_open("keyboard", &keyboard) == ESP_OK);
    assert(solar_os_input_source_open("buttons", &buttons) == ESP_OK);
    assert(keyboard != buttons);

    assert(solar_os_input_write_key(keyboard,
                                    0x1d,
                                    0x1d,
                                    'z',
                                    0,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    solar_os_input_key_event_t pressed[2];
    assert(solar_os_input_get_pressed(pressed, 2) == 1);
    assert(pressed[0].source == keyboard);
    assert(pressed[0].usage == 0x1d);
    assert(pressed[0].key == 'z');

    solar_os_input_key_event_t event = read_one_event();
    assert(event.action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(event.physical_key == 0x1d);

    assert(solar_os_input_write_key(keyboard,
                                    0x1d,
                                    0x1d,
                                    'z',
                                    0,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    assert(solar_os_input_read_events(&event, 1) == 0);

    now_us = 449000;
    assert(solar_os_input_read_events(&event, 1) == 0);
    now_us = 450000;
    event = read_one_event();
    assert(event.action == SOLAR_OS_INPUT_KEY_REPEAT);
    assert(event.key == 'z');

    assert(solar_os_input_write_key(keyboard,
                                    0x1d,
                                    0x1d,
                                    'z',
                                    0,
                                    SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
    assert(solar_os_input_get_pressed(pressed, 2) == 0);
    event = read_one_event();
    assert(event.action == SOLAR_OS_INPUT_KEY_RELEASE);

    assert(solar_os_input_write_key(keyboard,
                                    0x2b,
                                    0x2b,
                                    '\t',
                                    SOLAR_OS_INPUT_MOD_LEFT_ALT,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    assert(solar_os_input_write_char(buttons, 'b') == ESP_OK);
    char chars[3] = {0};
    assert(solar_os_input_read_source_chars(keyboard, chars, sizeof(chars)) == 2);
    assert((uint8_t)chars[0] == SOLAR_OS_KEY_ALT_PREFIX);
    assert(chars[1] == '\t');
    assert(solar_os_input_read_source_chars(buttons, chars, sizeof(chars)) == 1);
    assert(chars[0] == 'b');

    assert(solar_os_input_get_pressed(pressed, 2) == 1);
    solar_os_input_source_close(keyboard);
    assert(solar_os_input_get_pressed(pressed, 2) == 0);

    assert(solar_os_input_set_repeat(20, 300) == ESP_OK);
    uint16_t rate = 0;
    uint16_t delay = 0;
    solar_os_input_get_repeat(&rate, &delay);
    assert(rate == 20);
    assert(delay == 300);

    puts("input_test: ok");
    return 0;
}
