#include "solar_os_input.h"

#include <stdbool.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "solar_os_keys.h"

#define INPUT_SOURCE_MAX 8U
#define INPUT_SOURCE_NAME_MAX 16U
#define INPUT_QUEUE_MAX 64U
#define INPUT_REPEAT_RATE_DEFAULT 15U
#define INPUT_REPEAT_DELAY_DEFAULT_MS 450U
#define INPUT_NVS_NAMESPACE "input"
#define INPUT_NVS_REPEAT_RATE_KEY "repeat_cps"
#define INPUT_NVS_REPEAT_DELAY_KEY "repeat_delay"
#define INPUT_LEGACY_NVS_NAMESPACE "blekbd"

typedef struct {
    bool active;
    char name[INPUT_SOURCE_NAME_MAX];
} input_source_slot_t;

typedef struct {
    bool active;
    solar_os_input_key_event_t event;
} input_pressed_slot_t;

typedef struct {
    bool active;
    solar_os_input_source_t source;
    uint16_t physical_key;
    uint32_t next_ms;
} input_repeat_state_t;

static input_source_slot_t input_sources[INPUT_SOURCE_MAX];
static input_pressed_slot_t input_pressed[SOLAR_OS_INPUT_MAX_PRESSED_KEYS];
static solar_os_input_key_event_t input_queue[INPUT_QUEUE_MAX];
static size_t input_queue_head;
static size_t input_queue_count;
static uint16_t input_repeat_rate_cps = INPUT_REPEAT_RATE_DEFAULT;
static uint16_t input_repeat_delay_ms = INPUT_REPEAT_DELAY_DEFAULT_MS;
static input_repeat_state_t input_repeat;
static portMUX_TYPE input_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t input_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool input_source_valid_locked(solar_os_input_source_t source)
{
    return source > SOLAR_OS_INPUT_SOURCE_INVALID &&
        source <= INPUT_SOURCE_MAX &&
        input_sources[source - 1U].active;
}

static bool input_repeat_config_valid(uint16_t rate_cps, uint16_t delay_ms)
{
    if (rate_cps > SOLAR_OS_INPUT_REPEAT_RATE_MAX ||
        (rate_cps != 0 && rate_cps < SOLAR_OS_INPUT_REPEAT_RATE_MIN)) {
        return false;
    }
    return delay_ms >= SOLAR_OS_INPUT_REPEAT_DELAY_MIN_MS &&
        delay_ms <= SOLAR_OS_INPUT_REPEAT_DELAY_MAX_MS;
}

static uint32_t input_repeat_interval_ms(uint16_t rate_cps)
{
    return rate_cps == 0 ? 0 : (1000U + rate_cps - 1U) / rate_cps;
}

static bool input_key_repeatable(uint8_t key)
{
    return key != 0 &&
        key != SOLAR_OS_KEY_APP_EXIT &&
        key != SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE &&
        key != SOLAR_OS_KEY_ALT_PREFIX;
}

static input_pressed_slot_t *input_find_pressed_locked(solar_os_input_source_t source,
                                                        uint16_t physical_key)
{
    for (size_t i = 0; i < SOLAR_OS_INPUT_MAX_PRESSED_KEYS; i++) {
        if (input_pressed[i].active &&
            input_pressed[i].event.source == source &&
            input_pressed[i].event.physical_key == physical_key) {
            return &input_pressed[i];
        }
    }
    return NULL;
}

static input_pressed_slot_t *input_alloc_pressed_locked(void)
{
    for (size_t i = 0; i < SOLAR_OS_INPUT_MAX_PRESSED_KEYS; i++) {
        if (!input_pressed[i].active) {
            return &input_pressed[i];
        }
    }
    return NULL;
}

static bool input_queue_push_locked(const solar_os_input_key_event_t *event)
{
    if (event == NULL || input_queue_count >= INPUT_QUEUE_MAX) {
        return false;
    }
    const size_t index = (input_queue_head + input_queue_count) % INPUT_QUEUE_MAX;
    input_queue[index] = *event;
    input_queue_count++;
    return true;
}

static void input_repeat_stop_locked(solar_os_input_source_t source,
                                     uint16_t physical_key)
{
    if (input_repeat.active &&
        input_repeat.source == source &&
        input_repeat.physical_key == physical_key) {
        memset(&input_repeat, 0, sizeof(input_repeat));
    }
}

static void input_repeat_start_locked(const solar_os_input_key_event_t *event)
{
    if (event == NULL) {
        return;
    }
    if (!input_key_repeatable(event->key)) {
        if (event->key != 0) {
            memset(&input_repeat, 0, sizeof(input_repeat));
        }
        return;
    }
    if (input_repeat_rate_cps == 0) {
        return;
    }
    input_repeat = (input_repeat_state_t) {
        .active = true,
        .source = event->source,
        .physical_key = event->physical_key,
        .next_ms = input_now_ms() + input_repeat_delay_ms,
    };
}

static void input_queue_repeat_if_due_locked(void)
{
    if (!input_repeat.active || input_repeat_rate_cps == 0 ||
        (int32_t)(input_now_ms() - input_repeat.next_ms) < 0) {
        return;
    }

    input_pressed_slot_t *pressed =
        input_find_pressed_locked(input_repeat.source, input_repeat.physical_key);
    if (pressed == NULL) {
        memset(&input_repeat, 0, sizeof(input_repeat));
        return;
    }

    solar_os_input_key_event_t event = pressed->event;
    event.action = SOLAR_OS_INPUT_KEY_REPEAT;
    if (input_queue_push_locked(&event)) {
        input_repeat.next_ms = input_now_ms() +
            input_repeat_interval_ms(input_repeat_rate_cps);
    }
}

static esp_err_t input_load_repeat_namespace(const char *namespace_name,
                                             uint16_t *rate_cps,
                                             uint16_t *delay_ms)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(namespace_name, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u16(nvs, INPUT_NVS_REPEAT_RATE_KEY, rate_cps);
    if (err == ESP_OK) {
        err = nvs_get_u16(nvs, INPUT_NVS_REPEAT_DELAY_KEY, delay_ms);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t solar_os_input_init(void)
{
    uint16_t rate_cps = INPUT_REPEAT_RATE_DEFAULT;
    uint16_t delay_ms = INPUT_REPEAT_DELAY_DEFAULT_MS;
    esp_err_t err = input_load_repeat_namespace(INPUT_NVS_NAMESPACE,
                                                &rate_cps,
                                                &delay_ms);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = input_load_repeat_namespace(INPUT_LEGACY_NVS_NAMESPACE,
                                          &rate_cps,
                                          &delay_ms);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (!input_repeat_config_valid(rate_cps, delay_ms)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&input_lock);
    input_repeat_rate_cps = rate_cps;
    input_repeat_delay_ms = delay_ms;
    portEXIT_CRITICAL(&input_lock);
    return ESP_OK;
}

esp_err_t solar_os_input_source_open(const char *name, solar_os_input_source_t *source)
{
    if (name == NULL || name[0] == '\0' || source == NULL ||
        strlen(name) >= INPUT_SOURCE_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&input_lock);
    for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
        if (input_sources[i].active && strcmp(input_sources[i].name, name) == 0) {
            *source = (solar_os_input_source_t)(i + 1U);
            result = ESP_OK;
            break;
        }
    }
    if (result != ESP_OK) {
        for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
            if (input_sources[i].active) {
                continue;
            }
            input_sources[i].active = true;
            strlcpy(input_sources[i].name, name, sizeof(input_sources[i].name));
            *source = (solar_os_input_source_t)(i + 1U);
            result = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

void solar_os_input_source_release_all(solar_os_input_source_t source)
{
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source)) {
        portEXIT_CRITICAL(&input_lock);
        return;
    }

    for (size_t i = 0; i < SOLAR_OS_INPUT_MAX_PRESSED_KEYS; i++) {
        if (!input_pressed[i].active || input_pressed[i].event.source != source) {
            continue;
        }
        solar_os_input_key_event_t release = input_pressed[i].event;
        release.action = SOLAR_OS_INPUT_KEY_RELEASE;
        (void)input_queue_push_locked(&release);
        memset(&input_pressed[i], 0, sizeof(input_pressed[i]));
    }
    if (input_repeat.active && input_repeat.source == source) {
        memset(&input_repeat, 0, sizeof(input_repeat));
    }
    portEXIT_CRITICAL(&input_lock);
}

void solar_os_input_source_close(solar_os_input_source_t source)
{
    if (source == SOLAR_OS_INPUT_SOURCE_INVALID || source > INPUT_SOURCE_MAX) {
        return;
    }

    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source)) {
        portEXIT_CRITICAL(&input_lock);
        return;
    }

    solar_os_input_key_event_t retained[INPUT_QUEUE_MAX];
    size_t kept = 0;
    for (size_t i = 0; i < input_queue_count; i++) {
        const size_t read_index = (input_queue_head + i) % INPUT_QUEUE_MAX;
        if (input_queue[read_index].source != source) {
            retained[kept++] = input_queue[read_index];
        }
    }
    memcpy(input_queue, retained, kept * sizeof(retained[0]));
    input_queue_head = 0;
    input_queue_count = kept;
    for (size_t i = 0; i < SOLAR_OS_INPUT_MAX_PRESSED_KEYS; i++) {
        if (input_pressed[i].active && input_pressed[i].event.source == source) {
            memset(&input_pressed[i], 0, sizeof(input_pressed[i]));
        }
    }
    if (input_repeat.active && input_repeat.source == source) {
        memset(&input_repeat, 0, sizeof(input_repeat));
    }
    memset(&input_sources[source - 1U], 0, sizeof(input_sources[source - 1U]));
    portEXIT_CRITICAL(&input_lock);
}

esp_err_t solar_os_input_write_key(solar_os_input_source_t source,
                                   uint16_t physical_key,
                                   uint16_t usage,
                                   uint8_t key,
                                   uint8_t modifiers,
                                   solar_os_input_key_action_t action)
{
    if (physical_key == SOLAR_OS_INPUT_PHYSICAL_NONE ||
        action > SOLAR_OS_INPUT_KEY_REPEAT) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source)) {
        result = ESP_ERR_INVALID_STATE;
    } else if (action == SOLAR_OS_INPUT_KEY_PRESS) {
        input_pressed_slot_t *pressed = input_find_pressed_locked(source, physical_key);
        if (pressed == NULL) {
            pressed = input_alloc_pressed_locked();
        }
        if (pressed == NULL) {
            result = ESP_ERR_NO_MEM;
        } else if (!pressed->active) {
            pressed->active = true;
            pressed->event = (solar_os_input_key_event_t) {
                .source = source,
                .physical_key = physical_key,
                .usage = usage,
                .key = key,
                .modifiers = modifiers,
                .action = SOLAR_OS_INPUT_KEY_PRESS,
            };
            if (!input_queue_push_locked(&pressed->event)) {
                result = ESP_ERR_NO_MEM;
            }
            input_repeat_start_locked(&pressed->event);
        }
    } else if (action == SOLAR_OS_INPUT_KEY_RELEASE) {
        input_pressed_slot_t *pressed = input_find_pressed_locked(source, physical_key);
        if (pressed != NULL) {
            solar_os_input_key_event_t release = pressed->event;
            release.action = SOLAR_OS_INPUT_KEY_RELEASE;
            release.modifiers = modifiers;
            memset(pressed, 0, sizeof(*pressed));
            input_repeat_stop_locked(source, physical_key);
            if (!input_queue_push_locked(&release)) {
                result = ESP_ERR_NO_MEM;
            }
        }
    } else {
        input_pressed_slot_t *pressed = input_find_pressed_locked(source, physical_key);
        if (pressed == NULL) {
            result = ESP_ERR_NOT_FOUND;
        } else {
            solar_os_input_key_event_t repeat = pressed->event;
            repeat.action = SOLAR_OS_INPUT_KEY_REPEAT;
            repeat.modifiers = modifiers;
            if (!input_queue_push_locked(&repeat)) {
                result = ESP_ERR_NO_MEM;
            }
        }
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

esp_err_t solar_os_input_write_char(solar_os_input_source_t source, char ch)
{
    solar_os_input_key_event_t press = {
        .source = source,
        .physical_key = SOLAR_OS_INPUT_PHYSICAL_NONE,
        .usage = SOLAR_OS_INPUT_USAGE_NONE,
        .key = (uint8_t)ch,
        .modifiers = 0,
        .action = SOLAR_OS_INPUT_KEY_PRESS,
    };
    solar_os_input_key_event_t release = press;
    release.action = SOLAR_OS_INPUT_KEY_RELEASE;

    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source)) {
        result = ESP_ERR_INVALID_STATE;
    } else if (input_queue_count > INPUT_QUEUE_MAX - 2U) {
        result = ESP_ERR_NO_MEM;
    } else {
        (void)input_queue_push_locked(&press);
        (void)input_queue_push_locked(&release);
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

size_t solar_os_input_read_events(solar_os_input_key_event_t *events, size_t event_count)
{
    if (events == NULL || event_count == 0) {
        return 0;
    }

    portENTER_CRITICAL(&input_lock);
    input_queue_repeat_if_due_locked();
    size_t count = 0;
    while (count < event_count && input_queue_count > 0) {
        events[count++] = input_queue[input_queue_head];
        input_queue_head = (input_queue_head + 1U) % INPUT_QUEUE_MAX;
        input_queue_count--;
    }
    if (input_queue_count == 0) {
        input_queue_head = 0;
    }
    portEXIT_CRITICAL(&input_lock);
    return count;
}

static size_t input_read_chars_for_source(solar_os_input_source_t source,
                                          bool filter_source,
                                          char *buffer,
                                          size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return 0;
    }

    portENTER_CRITICAL(&input_lock);
    input_queue_repeat_if_due_locked();
    solar_os_input_key_event_t retained[INPUT_QUEUE_MAX];
    size_t kept = 0;
    size_t count = 0;
    for (size_t i = 0; i < input_queue_count; i++) {
        const size_t read_index = (input_queue_head + i) % INPUT_QUEUE_MAX;
        const solar_os_input_key_event_t event = input_queue[read_index];
        const bool selected = !filter_source || event.source == source;
        const bool emits_char =
            (event.action == SOLAR_OS_INPUT_KEY_PRESS ||
             event.action == SOLAR_OS_INPUT_KEY_REPEAT) &&
            event.key != 0;
        const bool emits_alt_prefix = emits_char &&
            ((((event.modifiers & SOLAR_OS_INPUT_MOD_ALT) != 0) && event.key == '\t') ||
             (((event.modifiers & SOLAR_OS_INPUT_MOD_LEFT_ALT) != 0) &&
              event.key != SOLAR_OS_KEY_APP_EXIT));
        const size_t needed = emits_char ? (emits_alt_prefix ? 2U : 1U) : 0U;
        if (!selected || count + needed > buffer_len) {
            retained[kept++] = event;
            continue;
        }
        if (emits_alt_prefix) {
            buffer[count++] = (char)SOLAR_OS_KEY_ALT_PREFIX;
        }
        if (emits_char) {
            buffer[count++] = (char)event.key;
        }
    }
    memcpy(input_queue, retained, kept * sizeof(retained[0]));
    input_queue_head = 0;
    input_queue_count = kept;
    portEXIT_CRITICAL(&input_lock);
    return count;
}

size_t solar_os_input_read_chars(char *buffer, size_t buffer_len)
{
    return input_read_chars_for_source(SOLAR_OS_INPUT_SOURCE_INVALID,
                                       false,
                                       buffer,
                                       buffer_len);
}

size_t solar_os_input_read_source_chars(solar_os_input_source_t source,
                                        char *buffer,
                                        size_t buffer_len)
{
    return input_read_chars_for_source(source, true, buffer, buffer_len);
}

size_t solar_os_input_get_pressed(solar_os_input_key_event_t *keys, size_t key_count)
{
    if (keys == NULL || key_count == 0) {
        return 0;
    }

    portENTER_CRITICAL(&input_lock);
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_INPUT_MAX_PRESSED_KEYS && count < key_count; i++) {
        if (input_pressed[i].active) {
            keys[count++] = input_pressed[i].event;
        }
    }
    portEXIT_CRITICAL(&input_lock);
    return count;
}

void solar_os_input_get_repeat(uint16_t *rate_cps, uint16_t *delay_ms)
{
    portENTER_CRITICAL(&input_lock);
    if (rate_cps != NULL) {
        *rate_cps = input_repeat_rate_cps;
    }
    if (delay_ms != NULL) {
        *delay_ms = input_repeat_delay_ms;
    }
    portEXIT_CRITICAL(&input_lock);
}

esp_err_t solar_os_input_set_repeat(uint16_t rate_cps, uint16_t delay_ms)
{
    if (delay_ms == 0) {
        solar_os_input_get_repeat(NULL, &delay_ms);
    }
    if (!input_repeat_config_valid(rate_cps, delay_ms)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&input_lock);
    input_repeat_rate_cps = rate_cps;
    input_repeat_delay_ms = delay_ms;
    if (rate_cps == 0) {
        memset(&input_repeat, 0, sizeof(input_repeat));
    }
    portEXIT_CRITICAL(&input_lock);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(INPUT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(nvs, INPUT_NVS_REPEAT_RATE_KEY, rate_cps);
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, INPUT_NVS_REPEAT_DELAY_KEY, delay_ms);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
