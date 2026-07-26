#include "solar_os_agent.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_agent_provider.h"
#include "solar_os_board.h"
#include "solar_os_json.h"
#include "solar_os_log.h"

#define AGENT_NVS_NAMESPACE "agent"
#define AGENT_NVS_ENDPOINT_KEY "endpoint"
#define AGENT_NVS_MODEL_KEY "model"
#define AGENT_NVS_API_KEY_KEY "api_key"
#define AGENT_NVS_REASONING_KEY "reasoning"
#define AGENT_DEFAULT_REASONING_EFFORT "medium"
#define AGENT_MAX_TOOL_ROUNDS 2U

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

typedef struct {
    solar_os_http_request_t *request;
    uint32_t refs;
    bool closing;
} agent_request_handle_t;

typedef struct {
    bool initialized;
    bool running;
    bool cancel_requested;
    solar_os_agent_provider_config_t config;
    agent_request_handle_t *active_request;
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
} agent_state_t;

static const char *TAG = "agent";
static portMUX_TYPE agent_lock = portMUX_INITIALIZER_UNLOCKED;
static agent_state_t agent;

static const solar_os_agent_tool_descriptor_t AGENT_TOOLS[] = {
    {
        .name = "system_status",
        .description =
            "Read the SolarOS board identity, uptime, firmware version, "
            "and current internal RAM and PSRAM availability.",
        .parameters_json =
            "{\"type\":\"object\",\"properties\":{},\"required\":[],"
            "\"additionalProperties\":false}",
        .strict = true,
    },
};

static uint32_t agent_internal_free(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint32_t agent_internal_largest(void)
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                       MALLOC_CAP_8BIT);
}

static uint32_t agent_psram_free(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool agent_string_valid(const char *text, size_t max_len, bool allow_empty)
{
    if (text == NULL) {
        return false;
    }
    const size_t len = strlen(text);
    if ((!allow_empty && len == 0) || len >= max_len) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool agent_endpoint_valid(const char *endpoint)
{
    return agent_string_valid(endpoint, SOLAR_OS_AGENT_ENDPOINT_MAX, false) &&
        (strncmp(endpoint, "http://", 7) == 0 ||
         strncmp(endpoint, "https://", 8) == 0);
}

static bool agent_reasoning_effort_valid(const char *effort)
{
    static const char * const values[] = {
        "none",
        "minimal",
        "low",
        "medium",
        "high",
        "xhigh",
        "max",
    };
    if (!agent_string_valid(effort,
                            SOLAR_OS_AGENT_REASONING_EFFORT_MAX,
                            false)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (strcmp(effort, values[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void agent_load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(AGENT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t len = sizeof(agent.config.endpoint);
    if (nvs_get_str(nvs,
                    AGENT_NVS_ENDPOINT_KEY,
                    agent.config.endpoint,
                    &len) != ESP_OK ||
        !agent_endpoint_valid(agent.config.endpoint)) {
        agent.config.endpoint[0] = '\0';
    }
    len = sizeof(agent.config.model);
    if (nvs_get_str(nvs,
                    AGENT_NVS_MODEL_KEY,
                    agent.config.model,
                    &len) != ESP_OK ||
        !agent_string_valid(agent.config.model,
                            sizeof(agent.config.model),
                            false)) {
        agent.config.model[0] = '\0';
    }
    len = sizeof(agent.config.api_key);
    if (nvs_get_str(nvs,
                    AGENT_NVS_API_KEY_KEY,
                    agent.config.api_key,
                    &len) != ESP_OK ||
        !agent_string_valid(agent.config.api_key,
                            sizeof(agent.config.api_key),
                            true)) {
        agent.config.api_key[0] = '\0';
    }
    len = sizeof(agent.config.reasoning_effort);
    if (nvs_get_str(nvs,
                    AGENT_NVS_REASONING_KEY,
                    agent.config.reasoning_effort,
                    &len) != ESP_OK ||
        !agent_reasoning_effort_valid(agent.config.reasoning_effort)) {
        strlcpy(agent.config.reasoning_effort,
                AGENT_DEFAULT_REASONING_EFFORT,
                sizeof(agent.config.reasoning_effort));
    }
    nvs_close(nvs);
}

esp_err_t solar_os_agent_init(void)
{
    portENTER_CRITICAL(&agent_lock);
    const bool initialized = agent.initialized;
    portEXIT_CRITICAL(&agent_lock);
    if (initialized) {
        return ESP_OK;
    }

    solar_os_agent_provider_config_t config = {0};
    strlcpy(config.reasoning_effort,
            AGENT_DEFAULT_REASONING_EFFORT,
            sizeof(config.reasoning_effort));
    portENTER_CRITICAL(&agent_lock);
    agent.config = config;
    portEXIT_CRITICAL(&agent_lock);
    agent_load_config();

    portENTER_CRITICAL(&agent_lock);
    agent.last_http_status = -1;
    agent.initialized = true;
    portEXIT_CRITICAL(&agent_lock);
    return ESP_OK;
}

static esp_err_t agent_save_string(const char *key,
                                   const char *value,
                                   char *cached,
                                   size_t cached_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AGENT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&agent_lock);
    strlcpy(cached, value, cached_len);
    portEXIT_CRITICAL(&agent_lock);
    return ESP_OK;
}

esp_err_t solar_os_agent_set_endpoint(const char *endpoint)
{
    if (!agent_endpoint_valid(endpoint)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_agent_init();
    return err == ESP_OK ?
        agent_save_string(AGENT_NVS_ENDPOINT_KEY,
                          endpoint,
                          agent.config.endpoint,
                          sizeof(agent.config.endpoint)) : err;
}

esp_err_t solar_os_agent_set_model(const char *model)
{
    if (!agent_string_valid(model, SOLAR_OS_AGENT_MODEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_agent_init();
    return err == ESP_OK ?
        agent_save_string(AGENT_NVS_MODEL_KEY,
                          model,
                          agent.config.model,
                          sizeof(agent.config.model)) : err;
}

esp_err_t solar_os_agent_set_api_key(const char *api_key)
{
    if (!agent_string_valid(api_key, SOLAR_OS_AGENT_API_KEY_MAX, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_agent_init();
    return err == ESP_OK ?
        agent_save_string(AGENT_NVS_API_KEY_KEY,
                          api_key,
                          agent.config.api_key,
                          sizeof(agent.config.api_key)) : err;
}

esp_err_t solar_os_agent_set_reasoning_effort(const char *effort)
{
    if (!agent_reasoning_effort_valid(effort)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_agent_init();
    return err == ESP_OK ?
        agent_save_string(AGENT_NVS_REASONING_KEY,
                          effort,
                          agent.config.reasoning_effort,
                          sizeof(agent.config.reasoning_effort)) : err;
}

esp_err_t solar_os_agent_forget(void)
{
    esp_err_t err = solar_os_agent_init();
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&agent_lock);
    const bool running = agent.running;
    portEXIT_CRITICAL(&agent_lock);
    if (running) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t nvs;
    err = nvs_open(AGENT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&agent_lock);
    memset(&agent.config, 0, sizeof(agent.config));
    strlcpy(agent.config.reasoning_effort,
            AGENT_DEFAULT_REASONING_EFFORT,
            sizeof(agent.config.reasoning_effort));
    portEXIT_CRITICAL(&agent_lock);
    return ESP_OK;
}

esp_err_t solar_os_agent_get_status(solar_os_agent_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)solar_os_agent_init();
    memset(status, 0, sizeof(*status));

    portENTER_CRITICAL(&agent_lock);
    status->initialized = agent.initialized;
    status->configured =
        agent.config.endpoint[0] != '\0' && agent.config.model[0] != '\0';
    status->api_key_set = agent.config.api_key[0] != '\0';
    status->running = agent.running;
    strlcpy(status->provider,
            solar_os_agent_openai_provider.name,
            sizeof(status->provider));
    strlcpy(status->endpoint, agent.config.endpoint, sizeof(status->endpoint));
    strlcpy(status->model, agent.config.model, sizeof(status->model));
    strlcpy(status->reasoning_effort,
            agent.config.reasoning_effort,
            sizeof(status->reasoning_effort));
    status->request_count = agent.request_count;
    status->failure_count = agent.failure_count;
    status->last_http_status = agent.last_http_status;
    status->last_error = agent.last_error;
    status->last_duration_ms = agent.last_duration_ms;
    status->last_bytes_received = agent.last_bytes_received;
    status->last_internal_before = agent.last_internal_before;
    status->last_internal_low = agent.last_internal_low;
    status->last_internal_after = agent.last_internal_after;
    status->last_internal_largest_before = agent.last_internal_largest_before;
    status->last_internal_largest_after = agent.last_internal_largest_after;
    status->last_psram_before = agent.last_psram_before;
    status->last_psram_after = agent.last_psram_after;
    portEXIT_CRITICAL(&agent_lock);
    return ESP_OK;
}

void solar_os_agent_provider_note_memory(void)
{
    const uint32_t current = agent_internal_free();
    portENTER_CRITICAL(&agent_lock);
    if (agent.running &&
        (agent.last_internal_low == 0 || current < agent.last_internal_low)) {
        agent.last_internal_low = current;
    }
    portEXIT_CRITICAL(&agent_lock);
}

bool solar_os_agent_provider_cancel_requested(void)
{
    portENTER_CRITICAL(&agent_lock);
    const bool cancel = agent.cancel_requested;
    portEXIT_CRITICAL(&agent_lock);
    return cancel;
}

void solar_os_agent_provider_bind_request(solar_os_http_request_t *request)
{
    if (request == NULL) {
        return;
    }
    static agent_request_handle_t handle;
    handle.request = request;
    handle.refs = 0;
    handle.closing = false;
    portENTER_CRITICAL(&agent_lock);
    agent.active_request = &handle;
    portEXIT_CRITICAL(&agent_lock);
}

void solar_os_agent_provider_unbind_request(solar_os_http_request_t *request)
{
    agent_request_handle_t *handle = NULL;
    portENTER_CRITICAL(&agent_lock);
    if (agent.active_request != NULL &&
        agent.active_request->request == request) {
        handle = agent.active_request;
        handle->closing = true;
        agent.active_request = NULL;
    }
    portEXIT_CRITICAL(&agent_lock);

    if (handle != NULL) {
        while (true) {
            portENTER_CRITICAL(&agent_lock);
            const uint32_t refs = handle->refs;
            portEXIT_CRITICAL(&agent_lock);
            if (refs == 0) {
                break;
            }
            vTaskDelay(1);
        }
        handle->request = NULL;
    }
}

esp_err_t solar_os_agent_cancel(void)
{
    agent_request_handle_t *handle = NULL;
    solar_os_http_request_t *request = NULL;
    portENTER_CRITICAL(&agent_lock);
    agent.cancel_requested = true;
    handle = agent.active_request;
    if (handle != NULL && !handle->closing && handle->request != NULL) {
        handle->refs++;
        request = handle->request;
    }
    portEXIT_CRITICAL(&agent_lock);

    esp_err_t err = ESP_OK;
    if (request != NULL) {
        err = solar_os_http_request_cancel(request);
        portENTER_CRITICAL(&agent_lock);
        handle->refs--;
        portEXIT_CRITICAL(&agent_lock);
    }
    return err;
}

static esp_err_t agent_emit(const solar_os_agent_request_t *request,
                            solar_os_agent_event_type_t type,
                            const char *text,
                            const char *tool_name,
                            bool success)
{
    solar_os_agent_event_t event = {
        .type = type,
        .success = success,
    };
    if (text != NULL) {
        strlcpy(event.text, text, sizeof(event.text));
    }
    if (tool_name != NULL) {
        strlcpy(event.tool_name, tool_name, sizeof(event.tool_name));
    }
    return request->event_handler(&event, request->user_data);
}

static esp_err_t agent_validate_object(const char *arguments)
{
    if (arguments == NULL || arguments[0] == '\0') {
        return ESP_OK;
    }
    solar_os_json_doc_t *doc = NULL;
    esp_err_t err = solar_os_json_parse_cstr(arguments, &doc);
    if (err == ESP_OK && !solar_os_json_is_object(solar_os_json_root(doc))) {
        err = ESP_ERR_INVALID_ARG;
    }
    solar_os_json_free(doc);
    return err;
}

static esp_err_t agent_execute_tool(const char *name,
                                    const char *arguments,
                                    char *result,
                                    size_t result_len)
{
    if (name == NULL || result == NULL || result_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(name, "system_status") != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = agent_validate_object(arguments);
    if (err != ESP_OK) {
        return err;
    }

    const uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    const int written = snprintf(
        result,
        result_len,
        "{\"board\":\"%s\",\"version\":\"%s\",\"uptime_ms\":%" PRIu64 ","
        "\"internal_free_bytes\":%" PRIu32 ","
        "\"internal_largest_block_bytes\":%" PRIu32 ","
        "\"psram_free_bytes\":%" PRIu32 "}",
        SOLAR_OS_BOARD_ID,
        SOLAR_OS_VERSION,
        uptime_ms,
        agent_internal_free(),
        agent_internal_largest(),
        agent_psram_free());
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void agent_finish_request(esp_err_t error,
                                 const solar_os_agent_provider_result_t *provider_result)
{
    const uint32_t internal_after = agent_internal_free();
    const uint32_t internal_largest_after = agent_internal_largest();
    const uint32_t psram_after = agent_psram_free();

    portENTER_CRITICAL(&agent_lock);
    agent.running = false;
    agent.cancel_requested = false;
    agent.last_error = error;
    agent.last_internal_after = internal_after;
    agent.last_internal_largest_after = internal_largest_after;
    agent.last_psram_after = psram_after;
    if (provider_result != NULL) {
        agent.last_http_status = provider_result->http_status;
        agent.last_duration_ms += provider_result->duration_ms;
        agent.last_bytes_received += provider_result->bytes_received;
    }
    if (error != ESP_OK) {
        agent.failure_count++;
    }
    portEXIT_CRITICAL(&agent_lock);
}

esp_err_t solar_os_agent_run(const solar_os_agent_request_t *request)
{
    if (request == NULL || request->prompt == NULL ||
        request->event_handler == NULL ||
        !agent_string_valid(request->prompt, SOLAR_OS_AGENT_PROMPT_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_agent_init();
    if (err != ESP_OK) {
        return err;
    }

    const uint32_t internal_before = agent_internal_free();
    const uint32_t internal_largest_before = agent_internal_largest();
    const uint32_t psram_before = agent_psram_free();
    solar_os_agent_provider_config_t config;
    portENTER_CRITICAL(&agent_lock);
    if (agent.running) {
        portEXIT_CRITICAL(&agent_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (agent.config.endpoint[0] == '\0' || agent.config.model[0] == '\0') {
        portEXIT_CRITICAL(&agent_lock);
        return ESP_ERR_INVALID_STATE;
    }
    config = agent.config;
    agent.running = true;
    agent.cancel_requested = false;
    agent.request_count++;
    agent.last_http_status = -1;
    agent.last_error = ESP_OK;
    agent.last_duration_ms = 0;
    agent.last_bytes_received = 0;
    agent.last_internal_before = internal_before;
    agent.last_internal_low = agent.last_internal_before;
    agent.last_internal_largest_before = internal_largest_before;
    agent.last_psram_before = psram_before;
    portEXIT_CRITICAL(&agent_lock);

    (void)agent_emit(request,
                     SOLAR_OS_AGENT_EVENT_STATUS,
                     "connecting",
                     NULL,
                     false);

    solar_os_agent_provider_turn_t turn = {
        .prompt = request->prompt,
        .tools = AGENT_TOOLS,
        .tool_count = sizeof(AGENT_TOOLS) / sizeof(AGENT_TOOLS[0]),
    };
    solar_os_agent_provider_result_t provider_result;
    char tool_result[SOLAR_OS_AGENT_TOOL_RESULT_MAX];
    char tool_call_id[SOLAR_OS_AGENT_TOOL_CALL_ID_MAX];
    char tool_name[SOLAR_OS_AGENT_TOOL_NAME_MAX];
    char tool_arguments[SOLAR_OS_AGENT_TOOL_ARGUMENTS_MAX];
    char previous_response_id[SOLAR_OS_AGENT_RESPONSE_ID_MAX];
    previous_response_id[0] = '\0';

    for (uint32_t round = 0; round < AGENT_MAX_TOOL_ROUNDS; round++) {
        memset(&provider_result, 0, sizeof(provider_result));
        provider_result.http_status = -1;
        err = solar_os_agent_openai_provider.run_turn(&config,
                                                       &turn,
                                                       request->event_handler,
                                                       request->user_data,
                                                       &provider_result);
        portENTER_CRITICAL(&agent_lock);
        agent.last_http_status = provider_result.http_status;
        agent.last_duration_ms += provider_result.duration_ms;
        agent.last_bytes_received += provider_result.bytes_received;
        portEXIT_CRITICAL(&agent_lock);
        if (err != ESP_OK) {
            break;
        }
        if (!provider_result.tool_call) {
            err = ESP_OK;
            break;
        }
        if (round + 1U >= AGENT_MAX_TOOL_ROUNDS) {
            err = ESP_ERR_INVALID_STATE;
            (void)agent_emit(request,
                             SOLAR_OS_AGENT_EVENT_ERROR,
                             "tool round limit reached",
                             provider_result.tool_name,
                             false);
            break;
        }

        (void)agent_emit(request,
                         SOLAR_OS_AGENT_EVENT_TOOL_CALL,
                         provider_result.tool_arguments,
                         provider_result.tool_name,
                         false);
        err = agent_execute_tool(provider_result.tool_name,
                                 provider_result.tool_arguments,
                                 tool_result,
                                 sizeof(tool_result));
        if (err != ESP_OK) {
            (void)agent_emit(request,
                             SOLAR_OS_AGENT_EVENT_ERROR,
                             err == ESP_ERR_NOT_SUPPORTED ?
                                 "tool is not available" : "tool execution failed",
                             provider_result.tool_name,
                             false);
            break;
        }
        (void)agent_emit(request,
                         SOLAR_OS_AGENT_EVENT_TOOL_RESULT,
                         tool_result,
                         provider_result.tool_name,
                         true);
        strlcpy(tool_call_id,
                provider_result.tool_call_id,
                sizeof(tool_call_id));
        strlcpy(tool_name,
                provider_result.tool_name,
                sizeof(tool_name));
        strlcpy(tool_arguments,
                provider_result.tool_arguments,
                sizeof(tool_arguments));
        strlcpy(previous_response_id,
                provider_result.response_id,
                sizeof(previous_response_id));
        turn.continuation = true;
        turn.previous_response_id = previous_response_id;
        turn.tool_call_id = tool_call_id;
        turn.tool_name = tool_name;
        turn.tool_arguments = tool_arguments;
        turn.tool_result = tool_result;
    }

    const bool cancelled = solar_os_agent_provider_cancel_requested();
    if (cancelled) {
        err = ESP_ERR_INVALID_STATE;
    }
    agent_finish_request(err, NULL);
    (void)agent_emit(request,
                     SOLAR_OS_AGENT_EVENT_DONE,
                     err == ESP_OK ? "complete" :
                         (cancelled ? "cancelled" : esp_err_to_name(err)),
                     NULL,
                     err == ESP_OK);
    memset(config.api_key, 0, sizeof(config.api_key));
    memset(tool_result, 0, sizeof(tool_result));
    memset(tool_call_id, 0, sizeof(tool_call_id));
    memset(tool_name, 0, sizeof(tool_name));
    memset(tool_arguments, 0, sizeof(tool_arguments));
    memset(previous_response_id, 0, sizeof(previous_response_id));
    SOLAR_OS_LOGI(TAG,
                  "request done: err=%s http=%d duration=%" PRIu32
                  " internal=%" PRIu32 " low=%" PRIu32 " after=%" PRIu32,
                  esp_err_to_name(err),
                  provider_result.http_status,
                  agent.last_duration_ms,
                  agent.last_internal_before,
                  agent.last_internal_low,
                  agent.last_internal_after);
    return err;
}
