#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef uint8_t solar_os_input_source_t;

#define SOLAR_OS_INPUT_SOURCE_INVALID 0U

esp_err_t solar_os_input_source_open(const char *name, solar_os_input_source_t *source);
void solar_os_input_source_close(solar_os_input_source_t source);
esp_err_t solar_os_input_write_char(solar_os_input_source_t source, char ch);
size_t solar_os_input_read_chars(char *buffer, size_t buffer_len);
