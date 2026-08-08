#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_port.h"
#include "solar_os_storage.h"

#define SOLAR_OS_STREAM_ID_MAX 24
#define SOLAR_OS_STREAM_UNIT_MAX 16
#define SOLAR_OS_STREAM_FORMAT_MAX 16
#define SOLAR_OS_STREAM_SUMMARY_MAX 64
#define SOLAR_OS_STREAM_CSV_HEADER_MAX 96
#define SOLAR_OS_STREAM_CSV_LINE_MAX 256
#define SOLAR_OS_STREAM_CHANGE_KEY_MAX 96

typedef enum {
    SOLAR_OS_STREAM_TYPE_SCALAR,
    SOLAR_OS_STREAM_TYPE_EVENT,
    SOLAR_OS_STREAM_TYPE_BYTES,
} solar_os_stream_type_t;

typedef struct {
    char id[SOLAR_OS_STREAM_ID_MAX];
    solar_os_stream_type_t type;
    char unit[SOLAR_OS_STREAM_UNIT_MAX];
    char format[SOLAR_OS_STREAM_FORMAT_MAX];
    char summary[SOLAR_OS_STREAM_SUMMARY_MAX];
} solar_os_stream_info_t;

typedef struct {
    char id[SOLAR_OS_STREAM_ID_MAX];
    solar_os_stream_type_t type;
    solar_os_port_handle_t port;
} solar_os_stream_handle_t;

typedef struct {
    uint32_t window_ms;
    uint32_t timeout_ms;
} solar_os_stream_read_options_t;

typedef struct {
    bool has_data;
    bool time_valid;
    uint64_t time_ms;
    uint64_t uptime_ms;
    char line[SOLAR_OS_STREAM_CSV_LINE_MAX];
    char change_key[SOLAR_OS_STREAM_CHANGE_KEY_MAX];
} solar_os_stream_csv_record_t;

#define SOLAR_OS_STREAM_HANDLE_INIT { \
    .id = "", \
    .type = SOLAR_OS_STREAM_TYPE_SCALAR, \
    .port = SOLAR_OS_PORT_HANDLE_INIT, \
}

size_t solar_os_stream_count(void);
bool solar_os_stream_get(size_t index, solar_os_stream_info_t *info);
esp_err_t solar_os_stream_get_info(const char *id, solar_os_stream_info_t *info);
const char *solar_os_stream_type_name(solar_os_stream_type_t type);
esp_err_t solar_os_stream_open(const char *id,
                               const char *owner,
                               solar_os_stream_handle_t *handle);
void solar_os_stream_close(solar_os_stream_handle_t *handle);
esp_err_t solar_os_stream_csv_header(const solar_os_stream_info_t *info,
                                     char *header,
                                     size_t header_len);
esp_err_t solar_os_stream_read_csv(solar_os_stream_handle_t *handle,
                                   const solar_os_stream_read_options_t *options,
                                   solar_os_stream_csv_record_t *record);

/* Read the stream's primary numeric value in the unit reported by info. */
esp_err_t solar_os_stream_read_scalar(
    solar_os_stream_handle_t *handle,
    const solar_os_stream_read_options_t *options,
    float *value);
