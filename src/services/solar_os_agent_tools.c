#include "solar_os_agent_tools.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "solar_os_board.h"
#include "solar_os_jobs.h"
#include "solar_os_json.h"
#include "solar_os_storage.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#define AGENT_TOOL_STORAGE_ENTRY_MAX 16U
#define AGENT_TOOL_JSON_SCRATCH_MAX 512U

#define AGENT_TOOL_SCHEMA_EMPTY \
    "{\"type\":\"object\",\"properties\":{},\"required\":[]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_STORAGE_LIST \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":159}},\"required\":[\"path\"]," \
    "\"additionalProperties\":false}"

#define AGENT_TOOL_OUTPUT_SYSTEM_STATUS \
    "{\"type\":\"object\",\"properties\":{\"board\":{\"type\":\"string\"}," \
    "\"version\":{\"type\":\"string\"},\"uptime_ms\":{\"type\":\"integer\"}," \
    "\"internal_free_bytes\":{\"type\":\"integer\"}," \
    "\"internal_largest_block_bytes\":{\"type\":\"integer\"}," \
    "\"psram_free_bytes\":{\"type\":\"integer\"}}," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_STORAGE_LIST \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}," \
    "\"entries\":{\"type\":\"array\"},\"truncated\":{\"type\":\"boolean\"}}," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_JOBS_LIST \
    "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\"}," \
    "\"jobs\":{\"type\":\"array\"},\"truncated\":{\"type\":\"boolean\"}}," \
    "\"additionalProperties\":false}"

typedef esp_err_t (*agent_tool_execute_fn)(const char *arguments,
                                           char *result,
                                           size_t result_len);
typedef bool (*agent_tool_available_fn)(void);

typedef struct {
    solar_os_agent_tool_descriptor_t provider;
    const char *domain;
    const char *output_schema_json;
    const char *required_capability;
    solar_os_agent_tool_risk_t risk;
    agent_tool_available_fn available;
    agent_tool_execute_fn execute;
} agent_tool_definition_t;

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} agent_tool_output_t;

static esp_err_t agent_tool_system_status(const char *arguments,
                                           char *result,
                                           size_t result_len);
static esp_err_t agent_tool_storage_list(const char *arguments,
                                         char *result,
                                         size_t result_len);
static esp_err_t agent_tool_jobs_list(const char *arguments,
                                      char *result,
                                      size_t result_len);

static bool agent_tool_storage_available(void)
{
    return solar_os_storage_is_mounted();
}

static const agent_tool_definition_t AGENT_TOOL_REGISTRY[] = {
    {
        .provider = {
            .name = "system_status",
            .description =
                "Read the SolarOS board identity, uptime, firmware version, "
                "and current internal RAM and PSRAM availability.",
            .parameters_json = AGENT_TOOL_SCHEMA_EMPTY,
            .strict = true,
        },
        .domain = "system",
        .output_schema_json = AGENT_TOOL_OUTPUT_SYSTEM_STATUS,
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .execute = agent_tool_system_status,
    },
    {
        .provider = {
            .name = "storage_list",
            .description =
                "List up to 16 entries in one absolute SolarOS storage "
                "directory. This reads names, types, and sizes only.",
            .parameters_json = AGENT_TOOL_SCHEMA_STORAGE_LIST,
            .strict = true,
        },
        .domain = "storage",
        .output_schema_json = AGENT_TOOL_OUTPUT_STORAGE_LIST,
        .required_capability = "storage",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .available = agent_tool_storage_available,
        .execute = agent_tool_storage_list,
    },
    {
        .provider = {
            .name = "jobs_list",
            .description =
                "List SolarOS background jobs with state, last error, and "
                "worker-stack memory requirements.",
            .parameters_json = AGENT_TOOL_SCHEMA_EMPTY,
            .strict = true,
        },
        .domain = "jobs",
        .output_schema_json = AGENT_TOOL_OUTPUT_JOBS_LIST,
        .risk = SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY,
        .execute = agent_tool_jobs_list,
    },
};

static const size_t AGENT_TOOL_COUNT =
    sizeof(AGENT_TOOL_REGISTRY) / sizeof(AGENT_TOOL_REGISTRY[0]);

static bool agent_tool_is_available(const agent_tool_definition_t *definition)
{
    return definition != NULL &&
        (definition->available == NULL || definition->available());
}

static esp_err_t agent_tool_parse_object(const char *arguments,
                                         solar_os_json_doc_t **doc,
                                         const solar_os_json_value_t **root)
{
    if (arguments == NULL || doc == NULL || root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *doc = NULL;
    *root = NULL;
    const char *source = arguments[0] == '\0' ? "{}" : arguments;
    esp_err_t err = solar_os_json_parse_cstr(source, doc);
    if (err != ESP_OK) {
        return err;
    }
    *root = solar_os_json_root(*doc);
    if (!solar_os_json_is_object(*root)) {
        solar_os_json_free(*doc);
        *doc = NULL;
        *root = NULL;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t agent_tool_output_append(agent_tool_output_t *output,
                                          const char *format,
                                          ...)
{
    if (output == NULL || output->buffer == NULL ||
        output->length >= output->capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(output->buffer + output->length,
                                  output->capacity - output->length,
                                  format,
                                  args);
    va_end(args);
    if (written < 0 ||
        (size_t)written >= output->capacity - output->length) {
        return ESP_ERR_INVALID_SIZE;
    }
    output->length += (size_t)written;
    return ESP_OK;
}

static esp_err_t agent_tool_validate_result(const char *result)
{
    solar_os_json_doc_t *doc = NULL;
    esp_err_t err = solar_os_json_parse_cstr(result, &doc);
    if (err == ESP_OK && !solar_os_json_is_object(solar_os_json_root(doc))) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    solar_os_json_free(doc);
    return err;
}

static esp_err_t agent_tool_system_status(const char *arguments,
                                           char *result,
                                           size_t result_len)
{
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }
    (void)root;
    solar_os_json_free(doc);

    const uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    const uint32_t internal_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internal_largest = (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t psram_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
        internal_free,
        internal_largest,
        psram_free);
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t agent_tool_storage_list(const char *arguments,
                                         char *result,
                                         size_t result_len)
{
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char requested[SOLAR_OS_STORAGE_PATH_MAX];
    const solar_os_json_value_t *path_value =
        solar_os_json_object_get(root, "path");
    err = solar_os_json_get_string(path_value, requested, sizeof(requested));
    if (err != ESP_OK || requested[0] != '/') {
        solar_os_json_free(doc);
        return ESP_ERR_INVALID_ARG;
    }
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    err = solar_os_storage_normalize_path(requested, path, sizeof(path));
    solar_os_json_free(doc);
    if (err != ESP_OK) {
        return err;
    }

    DIR *directory = opendir(path);
    if (directory == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char escaped_path[AGENT_TOOL_JSON_SCRATCH_MAX];
    err = solar_os_json_escape_string(path,
                                      escaped_path,
                                      sizeof(escaped_path));
    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "{\"path\":\"%s\",\"entries\":[",
                                       escaped_path);
    }

    bool first = true;
    bool truncated = false;
    size_t count = 0;
    struct dirent *entry = NULL;
    while (err == ESP_OK && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (count >= AGENT_TOOL_STORAGE_ENTRY_MAX) {
            truncated = true;
            break;
        }

        char escaped_name[AGENT_TOOL_JSON_SCRATCH_MAX];
        if (solar_os_json_escape_string(entry->d_name,
                                        escaped_name,
                                        sizeof(escaped_name)) != ESP_OK) {
            truncated = true;
            break;
        }
        char full_path[SOLAR_OS_STORAGE_PATH_MAX];
        const int path_written = strcmp(path, "/") == 0 ?
            snprintf(full_path,
                     sizeof(full_path),
                     "/%s",
                     entry->d_name) :
            snprintf(full_path,
                     sizeof(full_path),
                     "%s/%s",
                     path,
                     entry->d_name);
        struct stat status;
        memset(&status, 0, sizeof(status));
        const bool has_status =
            path_written >= 0 &&
            (size_t)path_written < sizeof(full_path) &&
            stat(full_path, &status) == 0;
        const char *type = !has_status ? "unknown" :
            (S_ISDIR(status.st_mode) ? "directory" :
             (S_ISREG(status.st_mode) ? "file" : "other"));

        char item[AGENT_TOOL_JSON_SCRATCH_MAX];
        const int item_written = snprintf(
            item,
            sizeof(item),
            "%s{\"name\":\"%s\",\"type\":\"%s\",\"size_bytes\":%" PRIu64 "}",
            first ? "" : ",",
            escaped_name,
            type,
            has_status ? (uint64_t)status.st_size : 0);
        const size_t tail_reserve = sizeof("],\"truncated\":true}");
        if (item_written < 0 ||
            (size_t)item_written >= sizeof(item) ||
            output.length + (size_t)item_written + tail_reserve >=
                output.capacity) {
            truncated = true;
            break;
        }
        err = agent_tool_output_append(&output, "%s", item);
        first = false;
        count++;
    }
    closedir(directory);
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "],\"truncated\":%s}",
                                       truncated ? "true" : "false");
    }
    return err;
}

static esp_err_t agent_tool_jobs_list(const char *arguments,
                                      char *result,
                                      size_t result_len)
{
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }
    (void)root;
    solar_os_json_free(doc);

    const size_t total = solar_os_jobs_count();
    agent_tool_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    err = agent_tool_output_append(&output,
                                   "{\"count\":%u,\"jobs\":[",
                                   (unsigned)total);
    bool first = true;
    bool truncated = false;
    for (size_t i = 0; err == ESP_OK && i < total; i++) {
        solar_os_job_status_t status;
        if (!solar_os_jobs_get(i, &status)) {
            continue;
        }
        char item[AGENT_TOOL_JSON_SCRATCH_MAX];
        const int written = snprintf(
            item,
            sizeof(item),
            "%s{\"name\":\"%s\",\"state\":\"%s\",\"last_error\":\"%s\","
            "\"memory_bytes\":%" PRIu32 ",\"memory_region\":\"%s\"}",
            first ? "" : ",",
            status.name,
            solar_os_job_state_name(status.state),
            esp_err_to_name(status.last_error),
            status.worker_stack_bytes,
            status.worker_stack_external ? "psram" : "internal");
        const size_t tail_reserve = sizeof("],\"truncated\":true}");
        if (written < 0 ||
            (size_t)written >= sizeof(item) ||
            output.length + (size_t)written + tail_reserve >= output.capacity) {
            truncated = true;
            break;
        }
        err = agent_tool_output_append(&output, "%s", item);
        first = false;
    }
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "],\"truncated\":%s}",
                                       truncated ? "true" : "false");
    }
    return err;
}

size_t solar_os_agent_tools_collect(
    solar_os_agent_tool_descriptor_t *descriptors,
    size_t capacity)
{
    if (descriptors == NULL || capacity == 0) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < AGENT_TOOL_COUNT && count < capacity; i++) {
        if (!agent_tool_is_available(&AGENT_TOOL_REGISTRY[i])) {
            continue;
        }
        descriptors[count++] = AGENT_TOOL_REGISTRY[i].provider;
    }
    return count;
}

size_t solar_os_agent_tools_count(void)
{
    return AGENT_TOOL_COUNT;
}

bool solar_os_agent_tools_get(size_t index, solar_os_agent_tool_info_t *info)
{
    if (index >= AGENT_TOOL_COUNT || info == NULL) {
        return false;
    }
    const agent_tool_definition_t *definition = &AGENT_TOOL_REGISTRY[index];
    *info = (solar_os_agent_tool_info_t){
        .provider = definition->provider,
        .domain = definition->domain,
        .output_schema_json = definition->output_schema_json,
        .required_capability = definition->required_capability,
        .risk = definition->risk,
        .available = agent_tool_is_available(definition),
    };
    return true;
}

esp_err_t solar_os_agent_tools_execute(const char *name,
                                       const char *arguments,
                                       char *result,
                                       size_t result_len)
{
    if (name == NULL || arguments == NULL ||
        result == NULL || result_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < AGENT_TOOL_COUNT; i++) {
        const agent_tool_definition_t *definition = &AGENT_TOOL_REGISTRY[i];
        if (strcmp(name, definition->provider.name) != 0) {
            continue;
        }
        if (!agent_tool_is_available(definition)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        result[0] = '\0';
        esp_err_t err = definition->execute(arguments, result, result_len);
        if (err == ESP_OK) {
            err = agent_tool_validate_result(result);
        }
        return err;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

const char *solar_os_agent_tool_risk_name(solar_os_agent_tool_risk_t risk)
{
    switch (risk) {
    case SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY:
        return "read-only";
    case SOLAR_OS_AGENT_TOOL_RISK_SENSITIVE_READ:
        return "sensitive-read";
    case SOLAR_OS_AGENT_TOOL_RISK_MUTATING:
        return "mutating";
    case SOLAR_OS_AGENT_TOOL_RISK_DISRUPTIVE:
        return "disruptive";
    default:
        return "unknown";
    }
}
