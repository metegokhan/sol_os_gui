#include "solar_os_agent.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_agent_provider.h"
#include "solar_os_agent_tools.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"

#define AGENT_NVS_NAMESPACE "agent"
#define AGENT_NVS_ENDPOINT_KEY "endpoint"
#define AGENT_NVS_MODEL_KEY "model"
#define AGENT_NVS_API_KEY_KEY "api_key"
#define AGENT_NVS_REASONING_KEY "reasoning"
#define AGENT_NVS_TOOL_POLICY_KEY "tool_policy"
#define AGENT_NVS_MAX_TOOLS_KEY "max_tools"
#define AGENT_DEFAULT_REASONING_EFFORT "medium"
#define AGENT_DEFAULT_TOOL_POLICY SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM

typedef struct {
    solar_os_http_request_t *request;
    uint32_t refs;
    bool closing;
} agent_request_handle_t;

typedef struct {
    solar_os_agent_provider_result_t provider_result;
    char tool_call_id[SOLAR_OS_AGENT_TOOL_CALL_ID_MAX];
    char tool_name[SOLAR_OS_AGENT_TOOL_NAME_MAX];
    char tool_arguments[SOLAR_OS_AGENT_TOOL_ARGUMENTS_MAX];
    char previous_response_id[SOLAR_OS_AGENT_RESPONSE_ID_MAX];
} agent_run_turn_state_t;

typedef struct {
    bool initialized;
    bool running;
    bool cancel_requested;
    solar_os_agent_provider_config_t config;
    solar_os_agent_tool_policy_t tool_policy;
    uint8_t max_tools;
    agent_request_handle_t *active_request;
    uint32_t request_count;
    uint32_t failure_count;
    uint32_t tool_executed_count;
    uint32_t tool_denied_count;
    uint32_t tool_failed_count;
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
static EXT_RAM_BSS_ATTR agent_state_t agent;

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

const char *solar_os_agent_tool_policy_name(
    solar_os_agent_tool_policy_t policy)
{
    switch (policy) {
    case SOLAR_OS_AGENT_TOOL_POLICY_OFF:
        return "off";
    case SOLAR_OS_AGENT_TOOL_POLICY_READONLY:
        return "readonly";
    case SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM:
        return "confirm";
    case SOLAR_OS_AGENT_TOOL_POLICY_ALL:
        return "all";
    default:
        return "unknown";
    }
}

esp_err_t solar_os_agent_parse_tool_policy(
    const char *name,
    solar_os_agent_tool_policy_t *policy)
{
    if (name == NULL || policy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const struct {
        const char *name;
        solar_os_agent_tool_policy_t policy;
    } values[] = {
        {"off", SOLAR_OS_AGENT_TOOL_POLICY_OFF},
        {"readonly", SOLAR_OS_AGENT_TOOL_POLICY_READONLY},
        {"confirm", SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM},
        {"all", SOLAR_OS_AGENT_TOOL_POLICY_ALL},
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (strcmp(name, values[i].name) == 0) {
            *policy = values[i].policy;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
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
    char tool_policy[16];
    len = sizeof(tool_policy);
    if (nvs_get_str(nvs,
                    AGENT_NVS_TOOL_POLICY_KEY,
                    tool_policy,
                    &len) != ESP_OK ||
        solar_os_agent_parse_tool_policy(tool_policy,
                                         &agent.tool_policy) != ESP_OK) {
        agent.tool_policy = AGENT_DEFAULT_TOOL_POLICY;
    }
    if (nvs_get_u8(nvs, AGENT_NVS_MAX_TOOLS_KEY, &agent.max_tools) != ESP_OK ||
        agent.max_tools < SOLAR_OS_AGENT_MAX_TOOLS_MIN ||
        agent.max_tools > SOLAR_OS_AGENT_MAX_TOOLS_MAX) {
        agent.max_tools = SOLAR_OS_AGENT_DEFAULT_MAX_TOOLS;
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
    agent.tool_policy = AGENT_DEFAULT_TOOL_POLICY;
    agent.max_tools = SOLAR_OS_AGENT_DEFAULT_MAX_TOOLS;
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

esp_err_t solar_os_agent_set_tool_policy(solar_os_agent_tool_policy_t policy)
{
    const char *name = solar_os_agent_tool_policy_name(policy);
    if (strcmp(name, "unknown") == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_agent_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open(AGENT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, AGENT_NVS_TOOL_POLICY_KEY, name);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&agent_lock);
        agent.tool_policy = policy;
        portEXIT_CRITICAL(&agent_lock);
    }
    return err;
}

esp_err_t solar_os_agent_set_max_tools(uint8_t max_tools)
{
    if (max_tools < SOLAR_OS_AGENT_MAX_TOOLS_MIN ||
        max_tools > SOLAR_OS_AGENT_MAX_TOOLS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_agent_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open(AGENT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs, AGENT_NVS_MAX_TOOLS_KEY, max_tools);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&agent_lock);
        agent.max_tools = max_tools;
        portEXIT_CRITICAL(&agent_lock);
    }
    return err;
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
    agent.tool_policy = AGENT_DEFAULT_TOOL_POLICY;
    agent.max_tools = SOLAR_OS_AGENT_DEFAULT_MAX_TOOLS;
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
    status->tool_policy = agent.tool_policy;
    status->max_tools = agent.max_tools;
    status->request_count = agent.request_count;
    status->failure_count = agent.failure_count;
    status->tool_executed_count = agent.tool_executed_count;
    status->tool_denied_count = agent.tool_denied_count;
    status->tool_failed_count = agent.tool_failed_count;
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

static bool agent_tool_info_by_name(const char *name,
                                    solar_os_agent_tool_info_t *info)
{
    if (name == NULL || info == NULL) {
        return false;
    }
    const size_t count = solar_os_agent_tools_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_agent_tool_info_t candidate;
        if (solar_os_agent_tools_get(i, &candidate) &&
            strcmp(name, candidate.provider.name) == 0) {
            *info = candidate;
            return true;
        }
    }
    return false;
}

static esp_err_t agent_authorize_tool(
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    const solar_os_agent_tool_info_t *info,
    const char *arguments,
    bool *allowed)
{
    if (request == NULL || info == NULL || arguments == NULL ||
        allowed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const solar_os_agent_tool_policy_decision_t decision =
        solar_os_agent_tools_policy_decision(policy, info->risk);
    *allowed = decision == SOLAR_OS_AGENT_TOOL_POLICY_ALLOW;
    if (decision != SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM_ONCE) {
        return ESP_OK;
    }
    if (request->confirm_tool == NULL) {
        return ESP_OK;
    }
    return request->confirm_tool(info->provider.name,
                                 solar_os_agent_tool_risk_name(info->risk),
                                 arguments,
                                 allowed,
                                 request->user_data);
}

static esp_err_t agent_denied_tool_result(
    const solar_os_agent_tool_info_t *info,
    solar_os_agent_tool_policy_t policy,
    char *result,
    size_t result_len)
{
    if (info == NULL || result == NULL || result_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(
        result,
        result_len,
        "{\"ok\":false,\"error\":\"tool denied\","
        "\"policy\":\"%s\",\"risk\":\"%s\"}",
        solar_os_agent_tool_policy_name(policy),
        solar_os_agent_tool_risk_name(info->risk));
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void agent_note_tool_result(bool denied, esp_err_t result)
{
    portENTER_CRITICAL(&agent_lock);
    if (denied) {
        agent.tool_denied_count++;
    } else if (result == ESP_OK) {
        agent.tool_executed_count++;
    } else {
        agent.tool_failed_count++;
    }
    portEXIT_CRITICAL(&agent_lock);
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
    solar_os_agent_tool_policy_t tool_policy;
    uint8_t max_tools;
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
    tool_policy = agent.tool_policy;
    max_tools = agent.max_tools;
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

    solar_os_agent_tool_descriptor_t
        tool_descriptors[SOLAR_OS_AGENT_TOOL_REGISTRY_MAX];
    const size_t tool_count =
        solar_os_agent_tools_collect(request,
                                     tool_policy,
                                     tool_descriptors,
                                     SOLAR_OS_AGENT_TOOL_REGISTRY_MAX);
    solar_os_agent_provider_turn_t turn = {
        .prompt = request->prompt,
        .previous_response_id = request->conversation_id,
        .tools = tool_descriptors,
        .tool_count = tool_count,
    };
    agent_run_turn_state_t *run = solar_os_memory_calloc(
        1,
        sizeof(*run),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.turn-state");
    char *tool_result =
        solar_os_memory_calloc(1,
                               SOLAR_OS_AGENT_TOOL_RESULT_MAX,
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "agent.tool-result");
    if (run == NULL || tool_result == NULL) {
        err = ESP_ERR_NO_MEM;
        agent_finish_request(err, NULL);
        (void)agent_emit(request,
                         SOLAR_OS_AGENT_EVENT_DONE,
                         esp_err_to_name(err),
                         NULL,
                         false);
        solar_os_memory_free(tool_result);
        solar_os_memory_free(run);
        memset(config.api_key, 0, sizeof(config.api_key));
        return err;
    }

    for (uint32_t turn_index = 0;
         turn_index <= (uint32_t)max_tools;
         turn_index++) {
        memset(&run->provider_result, 0, sizeof(run->provider_result));
        run->provider_result.http_status = -1;
        err = solar_os_agent_openai_provider.run_turn(&config,
                                                       &turn,
                                                       request->event_handler,
                                                       request->user_data,
                                                       &run->provider_result);
        portENTER_CRITICAL(&agent_lock);
        agent.last_http_status = run->provider_result.http_status;
        agent.last_duration_ms += run->provider_result.duration_ms;
        agent.last_bytes_received += run->provider_result.bytes_received;
        portEXIT_CRITICAL(&agent_lock);
        if (err != ESP_OK) {
            break;
        }
        if (!run->provider_result.tool_call) {
            err = ESP_OK;
            break;
        }
        if (turn_index >= (uint32_t)max_tools) {
            char limit_message[64];
            snprintf(limit_message,
                     sizeof(limit_message),
                     "tool call limit reached (max %u)",
                     (unsigned int)max_tools);
            err = ESP_ERR_INVALID_STATE;
            (void)agent_emit(request,
                             SOLAR_OS_AGENT_EVENT_ERROR,
                             limit_message,
                             run->provider_result.tool_name,
                             false);
            break;
        }

        (void)agent_emit(request,
                         SOLAR_OS_AGENT_EVENT_TOOL_CALL,
                         run->provider_result.tool_arguments,
                         run->provider_result.tool_name,
                         false);
        solar_os_agent_tool_info_t tool_info;
        if (!agent_tool_info_by_name(run->provider_result.tool_name,
                                     &tool_info)) {
            err = ESP_ERR_NOT_SUPPORTED;
            (void)agent_emit(request,
                             SOLAR_OS_AGENT_EVENT_ERROR,
                             "tool is not available",
                             run->provider_result.tool_name,
                             false);
            break;
        }
        bool allowed = false;
        err = agent_authorize_tool(request,
                                   tool_policy,
                                   &tool_info,
                                   run->provider_result.tool_arguments,
                                   &allowed);
        if (err != ESP_OK) {
            break;
        }
        if (!allowed) {
            err = agent_denied_tool_result(&tool_info,
                                           tool_policy,
                                           tool_result,
                                           SOLAR_OS_AGENT_TOOL_RESULT_MAX);
            agent_note_tool_result(true, err);
        } else {
            err = solar_os_agent_tools_execute(run->provider_result.tool_name,
                                               run->provider_result.tool_arguments,
                                               request,
                                               tool_policy,
                                               true,
                                               tool_result,
                                               SOLAR_OS_AGENT_TOOL_RESULT_MAX);
            agent_note_tool_result(false, err);
        }
        if (err != ESP_OK) {
            (void)agent_emit(request,
                             SOLAR_OS_AGENT_EVENT_ERROR,
                             err == ESP_ERR_NOT_SUPPORTED ?
                                 "tool is not available" :
                                 (err == ESP_ERR_NOT_ALLOWED ?
                                     "tool denied by policy" :
                                     "tool execution failed"),
                             run->provider_result.tool_name,
                             false);
            break;
        }
        (void)agent_emit(request,
                         SOLAR_OS_AGENT_EVENT_TOOL_RESULT,
                         tool_result,
                         run->provider_result.tool_name,
                         allowed);
        strlcpy(run->tool_call_id,
                run->provider_result.tool_call_id,
                sizeof(run->tool_call_id));
        strlcpy(run->tool_name,
                run->provider_result.tool_name,
                sizeof(run->tool_name));
        strlcpy(run->tool_arguments,
                run->provider_result.tool_arguments,
                sizeof(run->tool_arguments));
        strlcpy(run->previous_response_id,
                run->provider_result.response_id,
                sizeof(run->previous_response_id));
        turn.continuation = true;
        turn.previous_response_id = run->previous_response_id;
        turn.tool_call_id = run->tool_call_id;
        turn.tool_name = run->tool_name;
        turn.tool_arguments = run->tool_arguments;
        turn.tool_result = tool_result;
    }

    const bool cancelled = solar_os_agent_provider_cancel_requested();
    if (cancelled) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK && request->next_conversation_id != NULL &&
        request->next_conversation_id_len > 0U &&
        run->provider_result.response_id[0] != '\0') {
        strlcpy(request->next_conversation_id,
                run->provider_result.response_id,
                request->next_conversation_id_len);
    }
    const int last_http_status = run->provider_result.http_status;
    memset(tool_result, 0, SOLAR_OS_AGENT_TOOL_RESULT_MAX);
    solar_os_memory_free(tool_result);
    agent_finish_request(err, NULL);
    (void)agent_emit(request,
                     SOLAR_OS_AGENT_EVENT_DONE,
                     err == ESP_OK ? "complete" :
                         (cancelled ? "cancelled" : esp_err_to_name(err)),
                     NULL,
                     err == ESP_OK);
    memset(config.api_key, 0, sizeof(config.api_key));
    memset(run, 0, sizeof(*run));
    solar_os_memory_free(run);
    SOLAR_OS_LOGI(TAG,
                  "request done: err=%s http=%d duration=%" PRIu32
                  " internal=%" PRIu32 " low=%" PRIu32 " after=%" PRIu32,
                  esp_err_to_name(err),
                  last_http_status,
                  agent.last_duration_ms,
                  agent.last_internal_before,
                  agent.last_internal_low,
                  agent.last_internal_after);
    return err;
}
