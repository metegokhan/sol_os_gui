#include "solar_os_input.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#define INPUT_SOURCE_MAX 4U
#define INPUT_SOURCE_NAME_MAX 16U
#define INPUT_QUEUE_MAX 32U

typedef struct {
    bool active;
    char name[INPUT_SOURCE_NAME_MAX];
} input_source_slot_t;

typedef struct {
    solar_os_input_source_t source;
    char ch;
} input_queue_entry_t;

static input_source_slot_t input_sources[INPUT_SOURCE_MAX];
static input_queue_entry_t input_queue[INPUT_QUEUE_MAX];
static size_t input_queue_head;
static size_t input_queue_count;
static portMUX_TYPE input_lock = portMUX_INITIALIZER_UNLOCKED;

static bool input_source_valid_locked(solar_os_input_source_t source)
{
    return source > SOLAR_OS_INPUT_SOURCE_INVALID &&
        source <= INPUT_SOURCE_MAX &&
        input_sources[source - 1U].active;
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

    input_queue_entry_t retained[INPUT_QUEUE_MAX];
    size_t kept = 0;
    for (size_t i = 0; i < input_queue_count; i++) {
        const size_t read_index = (input_queue_head + i) % INPUT_QUEUE_MAX;
        if (input_queue[read_index].source == source) {
            continue;
        }
        retained[kept++] = input_queue[read_index];
    }
    memcpy(input_queue, retained, kept * sizeof(retained[0]));
    input_queue_head = 0;
    input_queue_count = kept;
    memset(&input_sources[source - 1U], 0, sizeof(input_sources[source - 1U]));
    portEXIT_CRITICAL(&input_lock);
}

esp_err_t solar_os_input_write_char(solar_os_input_source_t source, char ch)
{
    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source)) {
        result = ESP_ERR_INVALID_STATE;
    } else if (input_queue_count >= INPUT_QUEUE_MAX) {
        result = ESP_ERR_NO_MEM;
    } else {
        const size_t index = (input_queue_head + input_queue_count) % INPUT_QUEUE_MAX;
        input_queue[index] = (input_queue_entry_t) {
            .source = source,
            .ch = ch,
        };
        input_queue_count++;
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

size_t solar_os_input_read_chars(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return 0;
    }

    portENTER_CRITICAL(&input_lock);
    size_t count = 0;
    while (count < buffer_len && input_queue_count > 0) {
        buffer[count++] = input_queue[input_queue_head].ch;
        input_queue_head = (input_queue_head + 1U) % INPUT_QUEUE_MAX;
        input_queue_count--;
    }
    if (input_queue_count == 0) {
        input_queue_head = 0;
    }
    portEXIT_CRITICAL(&input_lock);
    return count;
}
