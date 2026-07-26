#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_AGENT_ENDPOINT_MAX 256
#define SOLAR_OS_AGENT_MODEL_MAX 96
#define SOLAR_OS_AGENT_API_KEY_MAX 192
#define SOLAR_OS_AGENT_PROMPT_MAX 1024
#define SOLAR_OS_AGENT_EVENT_TEXT_MAX 192
#define SOLAR_OS_AGENT_TOOL_NAME_MAX 48

typedef enum {
    SOLAR_OS_AGENT_EVENT_STATUS = 0,
    SOLAR_OS_AGENT_EVENT_TEXT_DELTA,
    SOLAR_OS_AGENT_EVENT_TOOL_CALL,
    SOLAR_OS_AGENT_EVENT_TOOL_RESULT,
    SOLAR_OS_AGENT_EVENT_USAGE,
    SOLAR_OS_AGENT_EVENT_ERROR,
    SOLAR_OS_AGENT_EVENT_DONE,
} solar_os_agent_event_type_t;

typedef struct {
    solar_os_agent_event_type_t type;
    char text[SOLAR_OS_AGENT_EVENT_TEXT_MAX];
    char tool_name[SOLAR_OS_AGENT_TOOL_NAME_MAX];
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;
    bool success;
} solar_os_agent_event_t;

typedef esp_err_t (*solar_os_agent_event_fn)(const solar_os_agent_event_t *event,
                                             void *user_data);

typedef struct {
    const char *prompt;
    solar_os_agent_event_fn event_handler;
    void *user_data;
} solar_os_agent_request_t;

typedef struct {
    bool initialized;
    bool configured;
    bool api_key_set;
    bool running;
    char provider[24];
    char endpoint[SOLAR_OS_AGENT_ENDPOINT_MAX];
    char model[SOLAR_OS_AGENT_MODEL_MAX];
    uint32_t request_count;
    uint32_t failure_count;
    int last_http_status;
    esp_err_t last_error;
    uint32_t last_duration_ms;
    uint32_t last_bytes_received;
    uint32_t last_internal_before;
    uint32_t last_internal_low;
    uint32_t last_internal_after;
    uint32_t last_internal_largest_before;
    uint32_t last_internal_largest_after;
    uint32_t last_psram_before;
    uint32_t last_psram_after;
} solar_os_agent_status_t;

esp_err_t solar_os_agent_init(void);
esp_err_t solar_os_agent_set_endpoint(const char *endpoint);
esp_err_t solar_os_agent_set_model(const char *model);
esp_err_t solar_os_agent_set_api_key(const char *api_key);
esp_err_t solar_os_agent_forget(void);
esp_err_t solar_os_agent_get_status(solar_os_agent_status_t *status);

/* Blocking; callers should use a foreground worker task. */
esp_err_t solar_os_agent_run(const solar_os_agent_request_t *request);
esp_err_t solar_os_agent_cancel(void);
