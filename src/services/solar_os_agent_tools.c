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
#include "solar_os_config.h"
#include "solar_os_jobs.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#define AGENT_TOOL_STORAGE_ENTRY_MAX 16U
#define AGENT_TOOL_STORAGE_CONTENT_MAX 3072U
#define AGENT_TOOL_JSON_SCRATCH_MAX 512U
#define AGENT_TOOL_SCRIPT_SOURCE_MAX 640U
#define AGENT_TOOL_SCRIPT_OUTPUT_MAX 384U
#define AGENT_TOOL_JSON_ESCAPE_FACTOR 6U

#define AGENT_TOOL_SCHEMA_EMPTY \
    "{\"type\":\"object\",\"properties\":{},\"required\":[]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_STORAGE_LIST \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":159}},\"required\":[\"path\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_STORAGE_READ AGENT_TOOL_SCHEMA_STORAGE_LIST
#define AGENT_TOOL_SCHEMA_STORAGE_WRITE \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":159},\"content\":{\"type\":\"string\"," \
    "\"maxLength\":3072}},\"required\":[\"path\",\"content\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_SCHEMA_SCRIPT_RUN \
    "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"," \
    "\"minLength\":1,\"maxLength\":640}},\"required\":[\"source\"]," \
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
#define AGENT_TOOL_OUTPUT_STORAGE_READ \
    "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}," \
    "\"path\":{\"type\":\"string\"},\"size_bytes\":{\"type\":\"integer\"}," \
    "\"content\":{\"type\":\"string\"},\"truncated\":{\"type\":\"boolean\"}}," \
    "\"required\":[\"ok\",\"path\",\"size_bytes\",\"content\",\"truncated\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_STORAGE_WRITE \
    "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}," \
    "\"path\":{\"type\":\"string\"},\"bytes_written\":{\"type\":\"integer\"}}," \
    "\"required\":[\"ok\",\"path\",\"bytes_written\"]," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_JOBS_LIST \
    "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\"}," \
    "\"jobs\":{\"type\":\"array\"},\"truncated\":{\"type\":\"boolean\"}}," \
    "\"additionalProperties\":false}"
#define AGENT_TOOL_OUTPUT_SCRIPT_RUN \
    "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}," \
    "\"status\":{\"type\":\"string\"},\"output\":{\"type\":\"string\"}," \
    "\"output_truncated\":{\"type\":\"boolean\"}," \
    "\"cancelled\":{\"type\":\"boolean\"}," \
    "\"timed_out\":{\"type\":\"boolean\"},\"error\":{\"type\":\"string\"}}," \
    "\"required\":[\"ok\",\"status\",\"output\",\"output_truncated\"," \
    "\"cancelled\",\"timed_out\",\"error\"]," \
    "\"additionalProperties\":false}"

typedef esp_err_t (*agent_tool_execute_fn)(const char *arguments,
                                           const solar_os_agent_request_t *request,
                                           char *result,
                                           size_t result_len);
typedef bool (*agent_tool_available_fn)(void);

typedef struct {
    solar_os_agent_tool_descriptor_t provider;
    const char *domain;
    const char *output_schema_json;
    const char *required_capability;
    solar_os_agent_tool_risk_t risk;
    uint32_t required_script_language;
    agent_tool_available_fn available;
    agent_tool_execute_fn execute;
} agent_tool_definition_t;

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} agent_tool_output_t;

static esp_err_t agent_tool_system_status(const char *arguments,
                                           const solar_os_agent_request_t *request,
                                           char *result,
                                           size_t result_len);
static esp_err_t agent_tool_storage_list(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len);
static esp_err_t agent_tool_storage_read(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len);
static esp_err_t agent_tool_storage_write(const char *arguments,
                                          const solar_os_agent_request_t *request,
                                          char *result,
                                          size_t result_len);
static esp_err_t agent_tool_jobs_list(const char *arguments,
                                      const solar_os_agent_request_t *request,
                                      char *result,
                                      size_t result_len);
static esp_err_t agent_tool_script_run_python(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len);
static esp_err_t agent_tool_script_run_lua(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len);

static bool agent_tool_storage_available(void)
{
    return solar_os_storage_is_mounted();
}

static bool agent_tool_python_available(void)
{
#if SOLAR_OS_PACKAGE_APP_PYTHON
    return true;
#else
    return false;
#endif
}

static bool agent_tool_lua_available(void)
{
#if SOLAR_OS_PACKAGE_APP_LUA
    return true;
#else
    return false;
#endif
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
            .name = "storage_read",
            .description =
                "Read one absolute SolarOS text file, except files below "
                ".ssh. The result is bounded and reports truncation.",
            .parameters_json = AGENT_TOOL_SCHEMA_STORAGE_READ,
            .strict = true,
        },
        .domain = "storage",
        .output_schema_json = AGENT_TOOL_OUTPUT_STORAGE_READ,
        .required_capability = "storage",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_SENSITIVE_READ,
        .available = agent_tool_storage_available,
        .execute = agent_tool_storage_read,
    },
    {
        .provider = {
            .name = "storage_write",
            .description =
                "Replace or create one absolute SolarOS text file "
                "with up to 3072 bytes, except files below .ssh.",
            .parameters_json = AGENT_TOOL_SCHEMA_STORAGE_WRITE,
            .strict = true,
        },
        .domain = "storage",
        .output_schema_json = AGENT_TOOL_OUTPUT_STORAGE_WRITE,
        .required_capability = "storage",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_MUTATING,
        .available = agent_tool_storage_available,
        .execute = agent_tool_storage_write,
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
    {
        .provider = {
            .name = "script_run_python",
            .description =
                "Execute a bounded MicroPython source string locally with "
                "SolarOS APIs. This can read or change device state and must "
                "be locally confirmed unless unrestricted tools are enabled.",
            .parameters_json = AGENT_TOOL_SCHEMA_SCRIPT_RUN,
            .strict = true,
        },
        .domain = "script",
        .output_schema_json = AGENT_TOOL_OUTPUT_SCRIPT_RUN,
        .required_capability = "python",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_DISRUPTIVE,
        .required_script_language = SOLAR_OS_AGENT_SCRIPT_PYTHON,
        .available = agent_tool_python_available,
        .execute = agent_tool_script_run_python,
    },
    {
        .provider = {
            .name = "script_run_lua",
            .description =
                "Execute a bounded Lua source string locally with SolarOS "
                "APIs. This can read or change device state and must be "
                "locally confirmed unless unrestricted tools are enabled.",
            .parameters_json = AGENT_TOOL_SCHEMA_SCRIPT_RUN,
            .strict = true,
        },
        .domain = "script",
        .output_schema_json = AGENT_TOOL_OUTPUT_SCRIPT_RUN,
        .required_capability = "lua",
        .risk = SOLAR_OS_AGENT_TOOL_RISK_DISRUPTIVE,
        .required_script_language = SOLAR_OS_AGENT_SCRIPT_LUA,
        .available = agent_tool_lua_available,
        .execute = agent_tool_script_run_lua,
    },
};

static const size_t AGENT_TOOL_COUNT =
    sizeof(AGENT_TOOL_REGISTRY) / sizeof(AGENT_TOOL_REGISTRY[0]);

static bool agent_tool_is_available(
    const agent_tool_definition_t *definition,
    const solar_os_agent_request_t *request)
{
    if (definition == NULL ||
        (definition->available != NULL && !definition->available())) {
        return false;
    }
    if (definition->required_script_language == 0U || request == NULL) {
        return true;
    }
    return request->run_script != NULL &&
        (request->script_languages & definition->required_script_language) != 0U;
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

static bool agent_tool_output_append_json_byte(agent_tool_output_t *output,
                                               uint8_t byte,
                                               size_t tail_reserve)
{
    char escaped[7];
    size_t length = 1U;
    escaped[0] = (char)byte;
    switch (byte) {
    case '"':
        memcpy(escaped, "\\\"", 2U);
        length = 2U;
        break;
    case '\\':
        memcpy(escaped, "\\\\", 2U);
        length = 2U;
        break;
    case '\b':
        memcpy(escaped, "\\b", 2U);
        length = 2U;
        break;
    case '\f':
        memcpy(escaped, "\\f", 2U);
        length = 2U;
        break;
    case '\n':
        memcpy(escaped, "\\n", 2U);
        length = 2U;
        break;
    case '\r':
        memcpy(escaped, "\\r", 2U);
        length = 2U;
        break;
    case '\t':
        memcpy(escaped, "\\t", 2U);
        length = 2U;
        break;
    default:
        if (byte < 0x20U) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
            length = 6U;
        }
        break;
    }
    if (output == NULL || output->buffer == NULL ||
        output->length + length + tail_reserve >= output->capacity) {
        return false;
    }
    memcpy(output->buffer + output->length, escaped, length);
    output->length += length;
    output->buffer[output->length] = '\0';
    return true;
}

static bool agent_tool_path_has_segment(const char *path,
                                        const char *segment)
{
    if (path == NULL || segment == NULL || segment[0] == '\0') {
        return false;
    }
    const size_t segment_len = strlen(segment);
    const char *cursor = path;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            cursor++;
        }
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }
        if ((size_t)(cursor - start) == segment_len &&
            memcmp(start, segment, segment_len) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t agent_tool_storage_path(
    const solar_os_json_value_t *root,
    char *path,
    size_t path_len)
{
    char requested[SOLAR_OS_STORAGE_PATH_MAX];
    const solar_os_json_value_t *path_value =
        solar_os_json_object_get(root, "path");
    esp_err_t err = solar_os_json_get_string(path_value,
                                              requested,
                                              sizeof(requested));
    if (err != ESP_OK || requested[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    err = solar_os_storage_normalize_path(requested, path, path_len);
    if (err == ESP_OK && agent_tool_path_has_segment(path, ".ssh")) {
        return ESP_ERR_NOT_ALLOWED;
    }
    return err;
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
                                           const solar_os_agent_request_t *request,
                                           char *result,
                                           size_t result_len)
{
    (void)request;
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
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len)
{
    (void)request;
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

static esp_err_t agent_tool_storage_read(const char *arguments,
                                         const solar_os_agent_request_t *request,
                                         char *result,
                                         size_t result_len)
{
    (void)request;
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    err = agent_tool_storage_path(root, path, sizeof(path));
    solar_os_json_free(doc);
    if (err != ESP_OK) {
        return err;
    }

    struct stat status;
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
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
        err = agent_tool_output_append(
            &output,
            "{\"ok\":true,\"path\":\"%s\",\"size_bytes\":%" PRIu64
            ",\"content\":\"",
            escaped_path,
            status.st_size > 0 ? (uint64_t)status.st_size : 0U);
    }

    const size_t tail_reserve = sizeof("\",\"truncated\":true}");
    size_t bytes_read = 0;
    bool truncated = false;
    while (err == ESP_OK && bytes_read < AGENT_TOOL_STORAGE_CONTENT_MAX) {
        const int byte = fgetc(file);
        if (byte == EOF) {
            if (ferror(file)) {
                err = ESP_FAIL;
            }
            break;
        }
        if (!agent_tool_output_append_json_byte(&output,
                                                (uint8_t)byte,
                                                tail_reserve)) {
            truncated = true;
            break;
        }
        bytes_read++;
    }
    if (err == ESP_OK && !truncated) {
        const int byte = fgetc(file);
        truncated = byte != EOF;
        if (byte == EOF && ferror(file)) {
            err = ESP_FAIL;
        }
    }
    fclose(file);
    if (err == ESP_OK) {
        err = agent_tool_output_append(&output,
                                       "\",\"truncated\":%s}",
                                       truncated ? "true" : "false");
    }
    return err;
}

static esp_err_t agent_tool_storage_write(const char *arguments,
                                          const solar_os_agent_request_t *request,
                                          char *result,
                                          size_t result_len)
{
    (void)request;
    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    err = agent_tool_storage_path(root, path, sizeof(path));
    char *content = solar_os_memory_calloc(
        1,
        AGENT_TOOL_STORAGE_CONTENT_MAX + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.storage-write");
    if (err == ESP_OK && content == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        const solar_os_json_value_t *content_value =
            solar_os_json_object_get(root, "content");
        err = solar_os_json_get_string(content_value,
                                       content,
                                       AGENT_TOOL_STORAGE_CONTENT_MAX + 1U);
    }
    solar_os_json_free(doc);
    if (err != ESP_OK) {
        solar_os_memory_free(content);
        return err;
    }

    struct stat status;
    if (stat(path, &status) == 0 && S_ISDIR(status.st_mode)) {
        solar_os_memory_free(content);
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        solar_os_memory_free(content);
        return ESP_FAIL;
    }
    const size_t content_len = strlen(content);
    const size_t written = fwrite(content, 1U, content_len, file);
    const int flush_result = fflush(file);
    const int close_result = fclose(file);
    const bool write_ok =
        written == content_len && flush_result == 0 && close_result == 0;
    if (!write_ok) {
        solar_os_memory_free(content);
        return ESP_FAIL;
    }

    char escaped_path[AGENT_TOOL_JSON_SCRATCH_MAX];
    err = solar_os_json_escape_string(path,
                                      escaped_path,
                                      sizeof(escaped_path));
    if (err == ESP_OK) {
        const int result_written = snprintf(
            result,
            result_len,
            "{\"ok\":true,\"path\":\"%s\",\"bytes_written\":%u}",
            escaped_path,
            (unsigned)written);
        err = result_written >= 0 && (size_t)result_written < result_len ?
            ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    memset(content, 0, AGENT_TOOL_STORAGE_CONTENT_MAX + 1U);
    solar_os_memory_free(content);
    return err;
}

static esp_err_t agent_tool_jobs_list(const char *arguments,
                                      const solar_os_agent_request_t *request,
                                      char *result,
                                      size_t result_len)
{
    (void)request;
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

static esp_err_t agent_tool_script_run(
    solar_os_agent_script_language_t language,
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    if (request == NULL || request->run_script == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    solar_os_json_doc_t *doc = NULL;
    const solar_os_json_value_t *root = NULL;
    esp_err_t err = agent_tool_parse_object(arguments, &doc, &root);
    if (err != ESP_OK) {
        return err;
    }

    char *source = solar_os_memory_calloc(
        1,
        AGENT_TOOL_SCRIPT_SOURCE_MAX + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.script-source");
    char *output = solar_os_memory_calloc(
        1,
        AGENT_TOOL_SCRIPT_OUTPUT_MAX,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.script-output");
    char *escaped_output = solar_os_memory_alloc(
        AGENT_TOOL_SCRIPT_OUTPUT_MAX * AGENT_TOOL_JSON_ESCAPE_FACTOR + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.script-json");
    char *escaped_error = solar_os_memory_alloc(
        SOLAR_OS_SCRIPT_ERROR_MAX * AGENT_TOOL_JSON_ESCAPE_FACTOR + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.tool.script-error");
    if (source == NULL || output == NULL ||
        escaped_output == NULL || escaped_error == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    const solar_os_json_value_t *source_value =
        solar_os_json_object_get(root, "source");
    err = solar_os_json_get_string(source_value,
                                   source,
                                   AGENT_TOOL_SCRIPT_SOURCE_MAX + 1U);
    solar_os_json_free(doc);
    doc = NULL;
    if (err != ESP_OK || source[0] == '\0') {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    solar_os_script_run_result_t run_result = {0};
    err = request->run_script(language,
                              source,
                              output,
                              AGENT_TOOL_SCRIPT_OUTPUT_MAX,
                              &run_result,
                              request->user_data);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = solar_os_json_escape_string(output,
                                      escaped_output,
                                      AGENT_TOOL_SCRIPT_OUTPUT_MAX *
                                          AGENT_TOOL_JSON_ESCAPE_FACTOR + 1U);
    if (err == ESP_OK) {
        err = solar_os_json_escape_string(
            run_result.error,
            escaped_error,
            SOLAR_OS_SCRIPT_ERROR_MAX * AGENT_TOOL_JSON_ESCAPE_FACTOR + 1U);
    }
    if (err != ESP_OK) {
        goto cleanup;
    }

    const int written = snprintf(
        result,
        result_len,
        "{\"ok\":%s,\"status\":\"%s\",\"output\":\"%s\","
        "\"output_truncated\":%s,\"cancelled\":%s,\"timed_out\":%s,"
        "\"error\":\"%s\"}",
        run_result.success ? "true" : "false",
        esp_err_to_name(run_result.status),
        escaped_output,
        run_result.output_truncated ? "true" : "false",
        run_result.cancelled ? "true" : "false",
        run_result.timed_out ? "true" : "false",
        escaped_error);
    err = written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;

cleanup:
    solar_os_json_free(doc);
    solar_os_memory_free(escaped_error);
    solar_os_memory_free(escaped_output);
    solar_os_memory_free(output);
    if (source != NULL) {
        memset(source, 0, AGENT_TOOL_SCRIPT_SOURCE_MAX + 1U);
    }
    solar_os_memory_free(source);
    return err;
}

static esp_err_t agent_tool_script_run_python(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    return agent_tool_script_run(SOLAR_OS_AGENT_SCRIPT_PYTHON,
                                 arguments,
                                 request,
                                 result,
                                 result_len);
}

static esp_err_t agent_tool_script_run_lua(
    const char *arguments,
    const solar_os_agent_request_t *request,
    char *result,
    size_t result_len)
{
    return agent_tool_script_run(SOLAR_OS_AGENT_SCRIPT_LUA,
                                 arguments,
                                 request,
                                 result,
                                 result_len);
}

solar_os_agent_tool_policy_decision_t solar_os_agent_tools_policy_decision(
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_risk_t risk)
{
    switch (policy) {
    case SOLAR_OS_AGENT_TOOL_POLICY_OFF:
        return SOLAR_OS_AGENT_TOOL_POLICY_DENY;
    case SOLAR_OS_AGENT_TOOL_POLICY_READONLY:
        return risk == SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY ?
            SOLAR_OS_AGENT_TOOL_POLICY_ALLOW :
            SOLAR_OS_AGENT_TOOL_POLICY_DENY;
    case SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM:
        return risk == SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY ?
            SOLAR_OS_AGENT_TOOL_POLICY_ALLOW :
            SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM_ONCE;
    case SOLAR_OS_AGENT_TOOL_POLICY_ALL:
        return SOLAR_OS_AGENT_TOOL_POLICY_ALLOW;
    default:
        return SOLAR_OS_AGENT_TOOL_POLICY_DENY;
    }
}

size_t solar_os_agent_tools_collect(
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_descriptor_t *descriptors,
    size_t capacity)
{
    if (descriptors == NULL || capacity == 0) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < AGENT_TOOL_COUNT && count < capacity; i++) {
        const agent_tool_definition_t *definition = &AGENT_TOOL_REGISTRY[i];
        if (!agent_tool_is_available(definition, request) ||
            solar_os_agent_tools_policy_decision(policy, definition->risk) ==
                SOLAR_OS_AGENT_TOOL_POLICY_DENY) {
            continue;
        }
        descriptors[count++] = definition->provider;
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
        .available = agent_tool_is_available(definition, NULL),
    };
    return true;
}

esp_err_t solar_os_agent_tools_execute(const char *name,
                                       const char *arguments,
                                       const solar_os_agent_request_t *request,
                                       solar_os_agent_tool_policy_t policy,
                                       bool confirmed,
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
        if (!agent_tool_is_available(definition, request)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        const solar_os_agent_tool_policy_decision_t decision =
            solar_os_agent_tools_policy_decision(policy, definition->risk);
        if (decision == SOLAR_OS_AGENT_TOOL_POLICY_DENY ||
            (decision == SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM_ONCE &&
             !confirmed)) {
            return ESP_ERR_NOT_ALLOWED;
        }
        result[0] = '\0';
        esp_err_t err =
            definition->execute(arguments, request, result, result_len);
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
