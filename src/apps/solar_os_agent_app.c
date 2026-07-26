#include "solar_os_agent_app.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_agent.h"
#include "solar_os_config.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_queue.h"
#include "solar_os_shell_io.h"
#include "solar_os_script_runner.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"
#include "solar_os_terminal.h"
#include "solar_os_wifi.h"
#if SOLAR_OS_PACKAGE_APP_LUA
#include "solar_os_lua.h"
#endif
#if SOLAR_OS_PACKAGE_APP_PYTHON
#include "solar_os_python.h"
#endif

#define AGENT_APP_TASK_STACK 16384U
#define AGENT_APP_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define AGENT_APP_EVENT_QUEUE_LEN 16U
#define AGENT_APP_SCRIPT_OUTPUT_MAX 4096U
#define AGENT_APP_SCRIPT_TIMEOUT_MS 30000U

SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(AGENT_APP_TASK_STACK);

typedef enum {
    AGENT_APP_MODE_ASK,
    AGENT_APP_MODE_SCRIPT_PYTHON,
    AGENT_APP_MODE_SCRIPT_LUA,
} agent_app_mode_t;

typedef struct {
    QueueHandle_t events;
    TaskHandle_t task;
    solar_os_context_t *ctx;
    char *prompt;
    char *script_output;
    agent_app_mode_t mode;
    solar_os_script_input_t script_input;
    int script_arg_start;
    volatile bool task_done;
    volatile bool stopping;
    bool running;
    bool text_started;
    bool script_reported;
} agent_app_state_t;

static const char *TAG = "agent_app";
static EXT_RAM_BSS_ATTR agent_app_state_t agent_app;
static EXT_RAM_BSS_ATTR solar_os_script_run_result_t agent_script_result;
static solar_os_shell_io_t agent_fallback_io;

static solar_os_shell_io_t *agent_app_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&agent_fallback_io,
                                        solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &agent_fallback_io);
        io = &agent_fallback_io;
    }
    return io;
}

static void agent_app_return_to_shell(solar_os_context_t *ctx)
{
    solar_os_context_request_terminal_preserve(ctx);
    solar_os_context_request_exit(ctx);
}

static void agent_app_print_status(solar_os_context_t *ctx)
{
    solar_os_agent_status_t status;
    solar_os_shell_io_t *io = agent_app_io(ctx);
    if (solar_os_agent_get_status(&status) != ESP_OK) {
        solar_os_shell_io_writeln(io, "agent: status unavailable");
        solar_os_shell_io_flush(io);
        return;
    }

    solar_os_shell_io_printf(io,
                             "Provider: %s\n"
                             "Endpoint: %s\n"
                             "Model: %s\n"
                             "API key: %s\n"
                             "Reasoning (Responses): %s\n"
                             "State: %s\n",
                             status.provider,
                             status.endpoint[0] != '\0' ?
                                 status.endpoint : "not configured",
                             status.model[0] != '\0' ?
                                 status.model : "not configured",
                             status.api_key_set ? "set" : "not set",
                             status.reasoning_effort,
                             status.running ? "running" : "idle");
    solar_os_shell_io_printf(io,
                             "Requests: %" PRIu32 ", failures: %" PRIu32 "\n",
                             status.request_count,
                             status.failure_count);
    if (status.request_count > 0) {
        solar_os_shell_io_printf(
            io,
            "Last: %s, HTTP %d, %" PRIu32 " ms, %" PRIu32 " bytes\n",
            esp_err_to_name(status.last_error),
            status.last_http_status,
            status.last_duration_ms,
            status.last_bytes_received);
        solar_os_shell_io_printf(
            io,
            "Internal: before %" PRIu32 ", low %" PRIu32
            ", request-end %" PRIu32 " bytes\n",
            status.last_internal_before,
            status.last_internal_low,
            status.last_internal_after);
        solar_os_shell_io_printf(
            io,
            "Largest internal: before %" PRIu32
            ", request-end %" PRIu32 " bytes\n",
            status.last_internal_largest_before,
            status.last_internal_largest_after);
        solar_os_shell_io_printf(
            io,
            "PSRAM: before %" PRIu32 ", request-end %" PRIu32 " bytes\n",
            status.last_psram_before,
            status.last_psram_after);
    }
    solar_os_shell_io_flush(io);
}

static esp_err_t agent_app_build_prompt(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc < 3) {
        return ESP_ERR_INVALID_ARG;
    }
    agent_app.prompt = solar_os_memory_calloc(1,
                                              SOLAR_OS_AGENT_PROMPT_MAX,
                                              SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                              "agent.app.prompt");
    if (agent_app.prompt == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t used = 0;
    for (int i = 2; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        const size_t len = strlen(arg);
        const size_t separator = used > 0 ? 1U : 0U;
        if (used + separator + len >= SOLAR_OS_AGENT_PROMPT_MAX) {
            solar_os_memory_free(agent_app.prompt);
            agent_app.prompt = NULL;
            return ESP_ERR_INVALID_SIZE;
        }
        if (separator != 0) {
            agent_app.prompt[used++] = ' ';
        }
        memcpy(agent_app.prompt + used, arg, len);
        used += len;
        agent_app.prompt[used] = '\0';
    }
    return used > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t agent_app_build_script(solar_os_context_t *ctx)
{
    if (solar_os_context_argc(ctx) < 4) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *language = solar_os_context_argv(ctx, 2);
    if (strcmp(language, "python") == 0) {
#if SOLAR_OS_PACKAGE_APP_PYTHON
        agent_app.mode = AGENT_APP_MODE_SCRIPT_PYTHON;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    } else if (strcmp(language, "lua") == 0) {
#if SOLAR_OS_PACKAGE_APP_LUA
        agent_app.mode = AGENT_APP_MODE_SCRIPT_LUA;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    const char *input = solar_os_context_argv(ctx, 3);
    esp_err_t err = ESP_OK;
    if (strcmp(input, "-c") == 0) {
        if (solar_os_context_argc(ctx) < 5) {
            return ESP_ERR_INVALID_ARG;
        }
        input = solar_os_context_argv(ctx, 4);
        const size_t input_size = strlen(input) + 1U;
        agent_app.prompt = solar_os_memory_alloc(
            input_size,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
            "agent.script.source");
        if (agent_app.prompt != NULL) {
            memcpy(agent_app.prompt, input, input_size);
        }
        agent_app.script_input = SOLAR_OS_SCRIPT_INPUT_SOURCE;
        agent_app.script_arg_start = 5;
    } else {
        agent_app.prompt = solar_os_memory_calloc(1,
                                                  SOLAR_OS_STORAGE_PATH_MAX,
                                                  SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                  "agent.script.path");
        if (agent_app.prompt != NULL) {
            err = solar_os_storage_resolve_path(input,
                                                agent_app.prompt,
                                                SOLAR_OS_STORAGE_PATH_MAX);
        }
        agent_app.script_input = SOLAR_OS_SCRIPT_INPUT_FILE;
        agent_app.script_arg_start = 4;
    }
    if (agent_app.prompt == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        return err;
    }

    agent_app.script_output = solar_os_memory_calloc(
        1,
        AGENT_APP_SCRIPT_OUTPUT_MAX,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.script.output");
    return agent_app.script_output != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool agent_app_script_cancel_requested(void *user)
{
    const agent_app_state_t *state = (const agent_app_state_t *)user;
    return state != NULL && state->stopping;
}

static bool agent_app_send_event(const solar_os_agent_event_t *event)
{
    if (event == NULL || agent_app.events == NULL) {
        return false;
    }
    while (!agent_app.task_done && !agent_app.stopping) {
        if (xQueueSend(agent_app.events, event, pdMS_TO_TICKS(100)) == pdPASS) {
            return true;
        }
    }
    return false;
}

static esp_err_t agent_app_service_event(const solar_os_agent_event_t *event,
                                         void *user_data)
{
    (void)user_data;
    return agent_app_send_event(event) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void agent_app_task(void *arg)
{
    (void)arg;
    if (agent_app.mode == AGENT_APP_MODE_ASK) {
        const solar_os_agent_request_t request = {
            .prompt = agent_app.prompt,
            .event_handler = agent_app_service_event,
            .user_data = &agent_app,
        };
        (void)solar_os_agent_run(&request);
    } else {
        const char *argv[SOLAR_OS_APP_ARG_MAX] = {
            agent_app.script_input == SOLAR_OS_SCRIPT_INPUT_SOURCE
                ? "<agent>"
                : agent_app.prompt,
        };
        int script_argc = 1;
        const int argc = solar_os_context_argc(agent_app.ctx);
        for (int i = agent_app.script_arg_start;
             i < argc && script_argc < SOLAR_OS_APP_ARG_MAX;
             i++) {
            argv[script_argc++] = solar_os_context_argv(agent_app.ctx, i);
        }
        const solar_os_script_run_request_t request = {
            .context = agent_app.ctx,
            .input_type = agent_app.script_input,
            .input = agent_app.prompt,
            .source_name = agent_app.script_input == SOLAR_OS_SCRIPT_INPUT_SOURCE
                ? "<agent>"
                : agent_app.prompt,
            .argc = script_argc,
            .argv = argv,
            .timeout_ms = AGENT_APP_SCRIPT_TIMEOUT_MS,
            .cancel_requested = agent_app_script_cancel_requested,
            .cancel_user = &agent_app,
            .output = agent_app.script_output,
            .output_size = AGENT_APP_SCRIPT_OUTPUT_MAX,
        };
#if SOLAR_OS_PACKAGE_APP_PYTHON
        if (agent_app.mode == AGENT_APP_MODE_SCRIPT_PYTHON) {
            (void)solar_os_python_run(&request, &agent_script_result);
        }
#endif
#if SOLAR_OS_PACKAGE_APP_LUA
        if (agent_app.mode == AGENT_APP_MODE_SCRIPT_LUA) {
            (void)solar_os_lua_run(&request, &agent_script_result);
        }
#endif
    }
    agent_app.task_done = true;
    solar_os_task_delete_internal(NULL);
}

static void agent_app_cleanup(void)
{
    if (agent_app.events != NULL) {
        solar_os_queue_delete(agent_app.events);
        agent_app.events = NULL;
    }
    if (agent_app.prompt != NULL) {
        solar_os_memory_free(agent_app.prompt);
        agent_app.prompt = NULL;
    }
    if (agent_app.script_output != NULL) {
        solar_os_memory_free(agent_app.script_output);
        agent_app.script_output = NULL;
    }
    agent_app.task = NULL;
    agent_app.running = false;
}

static void agent_app_print_delta(solar_os_shell_io_t *io,
                                  const char *text)
{
    if (text == NULL) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        solar_os_shell_io_put_utf8_byte(io, *p);
    }
}

static void agent_app_report_script(solar_os_context_t *ctx)
{
    if (agent_app.mode == AGENT_APP_MODE_ASK || !agent_app.task_done ||
        agent_app.script_reported) {
        return;
    }

    solar_os_shell_io_t *io = agent_app_io(ctx);
    if (agent_script_result.output_len > 0) {
        agent_app_print_delta(io, agent_app.script_output);
        if (agent_app.script_output[agent_script_result.output_len - 1] != '\n') {
            solar_os_shell_io_newline(io);
        }
    }
    if (agent_script_result.output_truncated) {
        solar_os_shell_io_writeln(io, "agent: script output truncated");
    }
    if (agent_script_result.success) {
        solar_os_shell_io_writeln(io, "agent: script complete");
    } else {
        solar_os_shell_io_printf(
            io,
            "agent: script failed: %s%s%s\n",
            esp_err_to_name(agent_script_result.status),
            agent_script_result.error[0] != '\0' ? ", " : "",
            agent_script_result.error);
    }
    solar_os_shell_io_flush(io);
    agent_app.script_reported = true;
    agent_app.running = false;
    agent_app_return_to_shell(ctx);
}

static void agent_app_drain_events(solar_os_context_t *ctx)
{
    if (agent_app.events == NULL) {
        return;
    }
    solar_os_shell_io_t *io = agent_app_io(ctx);
    solar_os_agent_event_t event;
    while (xQueueReceive(agent_app.events, &event, 0) == pdPASS) {
        switch (event.type) {
        case SOLAR_OS_AGENT_EVENT_STATUS:
            solar_os_shell_io_printf(io, "agent: %s\n", event.text);
            break;
        case SOLAR_OS_AGENT_EVENT_TEXT_DELTA:
            agent_app.text_started = true;
            agent_app_print_delta(io, event.text);
            break;
        case SOLAR_OS_AGENT_EVENT_TOOL_CALL:
            solar_os_shell_io_printf(io,
                                     "\nagent: tool %s\n",
                                     event.tool_name);
            break;
        case SOLAR_OS_AGENT_EVENT_TOOL_RESULT:
            solar_os_shell_io_printf(io,
                                     "agent: tool %s complete\n",
                                     event.tool_name);
            break;
        case SOLAR_OS_AGENT_EVENT_USAGE:
            solar_os_shell_io_printf(
                io,
                "\nagent: tokens %" PRIu32 " in, %" PRIu32
                " out, %" PRIu32 " total\n",
                event.prompt_tokens,
                event.completion_tokens,
                event.total_tokens);
            break;
        case SOLAR_OS_AGENT_EVENT_ERROR:
            solar_os_shell_io_printf(io, "\nagent: %s\n", event.text);
            break;
        case SOLAR_OS_AGENT_EVENT_DONE:
            if (agent_app.text_started) {
                solar_os_shell_io_newline(io);
            }
            solar_os_shell_io_printf(io,
                                     "agent: %s\n",
                                     event.success ? "complete" : event.text);
            agent_app_print_status(ctx);
            agent_app.running = false;
            agent_app_return_to_shell(ctx);
            break;
        default:
            break;
        }
    }
    solar_os_shell_io_flush(io);
}

static esp_err_t agent_app_start(solar_os_context_t *ctx)
{
    if (agent_app.task != NULL && !agent_app.task_done) {
        solar_os_shell_io_writeln(agent_app_io(ctx),
                                  "agent: previous request is still stopping");
        solar_os_shell_io_flush(agent_app_io(ctx));
        return ESP_OK;
    }
    agent_app_cleanup();
    memset(&agent_app, 0, sizeof(agent_app));
    memset(&agent_script_result, 0, sizeof(agent_script_result));
    agent_app.ctx = ctx;
    (void)solar_os_agent_init();

    const int argc = solar_os_context_argc(ctx);
    const bool ask_mode = argc >= 3 &&
        strcmp(solar_os_context_argv(ctx, 1), "ask") == 0;
    const bool script_mode = argc >= 4 &&
        strcmp(solar_os_context_argv(ctx, 1), "script") == 0;
    if (!ask_mode && !script_mode) {
        solar_os_shell_io_writeln(agent_app_io(ctx),
                                  "agent: launch with agent ask or script");
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_return_to_shell(ctx);
        return ESP_OK;
    }

    solar_os_agent_status_t status = {0};
    esp_err_t err;
    if (ask_mode) {
        agent_app.mode = AGENT_APP_MODE_ASK;
        solar_os_wifi_status_t wifi;
        solar_os_wifi_get_status(&wifi);
        if (!wifi.started || !wifi.connected || !wifi.has_ip) {
            solar_os_shell_io_writeln(agent_app_io(ctx),
                                      "agent: Wi-Fi is not connected");
            solar_os_shell_io_flush(agent_app_io(ctx));
            agent_app_return_to_shell(ctx);
            return ESP_OK;
        }
        (void)solar_os_agent_get_status(&status);
        if (!status.configured) {
            solar_os_shell_io_writeln(agent_app_io(ctx),
                                      "agent: configure endpoint and model first");
            solar_os_shell_io_flush(agent_app_io(ctx));
            agent_app_return_to_shell(ctx);
            return ESP_OK;
        }
        err = agent_app_build_prompt(ctx);
    } else {
        err = agent_app_build_script(ctx);
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(agent_app_io(ctx),
                                 "agent: invalid request: %s\n",
                                 esp_err_to_name(err));
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_cleanup();
        agent_app_return_to_shell(ctx);
        return ESP_OK;
    }
    if (ask_mode) {
        agent_app.events = solar_os_queue_create(AGENT_APP_EVENT_QUEUE_LEN,
                                                  sizeof(solar_os_agent_event_t));
        if (agent_app.events == NULL) {
            solar_os_shell_io_writeln(agent_app_io(ctx), "agent: out of memory");
            solar_os_shell_io_flush(agent_app_io(ctx));
            agent_app_cleanup();
            agent_app_return_to_shell(ctx);
            return ESP_OK;
        }
    }

    if (ask_mode) {
        solar_os_shell_io_printf_bold(agent_app_io(ctx),
                                      "agent (%s)\n",
                                      status.model);
    } else {
        solar_os_shell_io_printf_bold(
            agent_app_io(ctx),
            "agent script (%s)\n",
            agent_app.mode == AGENT_APP_MODE_SCRIPT_PYTHON ? "python" : "lua");
    }
    solar_os_shell_io_flush(agent_app_io(ctx));
    agent_app.running = true;
    const BaseType_t created = solar_os_task_create_pinned_internal(
        agent_app_task,
        "solar_os_agent",
        AGENT_APP_TASK_STACK,
        NULL,
        AGENT_APP_TASK_PRIORITY,
        &agent_app.task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        solar_os_shell_io_writeln(agent_app_io(ctx),
                                  "agent: task create failed");
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_cleanup();
        agent_app_return_to_shell(ctx);
    }
    return ESP_OK;
}

static void agent_app_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (agent_app.running && !agent_app.task_done) {
        agent_app.stopping = true;
        if (agent_app.mode == AGENT_APP_MODE_ASK) {
            (void)solar_os_agent_cancel();
        }
    }
    if (!solar_os_task_wait_done(agent_app.task,
                                 &agent_app.task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "agent task did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return;
    }
    agent_app_cleanup();
}

static bool agent_app_event(solar_os_context_t *ctx,
                            const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (agent_app.mode == AGENT_APP_MODE_ASK) {
            agent_app_drain_events(ctx);
        } else {
            agent_app_report_script(ctx);
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        if (agent_app.running) {
            solar_os_shell_io_writeln(agent_app_io(ctx),
                                      "\nagent: cancelling");
            solar_os_shell_io_flush(agent_app_io(ctx));
            agent_app.stopping = true;
            if (agent_app.mode == AGENT_APP_MODE_ASK) {
                (void)solar_os_agent_cancel();
            }
        }
        agent_app_return_to_shell(ctx);
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_UP) {
        solar_os_terminal_t *terminal =
            solar_os_shell_io_terminal(agent_app_io(ctx));
        if (terminal != NULL) {
            solar_os_terminal_page_up(terminal);
        }
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_DOWN) {
        solar_os_terminal_t *terminal =
            solar_os_shell_io_terminal(agent_app_io(ctx));
        if (terminal != NULL) {
            solar_os_terminal_page_down(terminal);
        }
        return true;
    }
    return true;
}

const solar_os_app_t solar_os_agent_app = {
    .name = "agent",
    .summary = "native LLM agent",
    .start = agent_app_start,
    .stop = agent_app_stop,
    .event = agent_app_event,
    .worker_stack_bytes = AGENT_APP_TASK_STACK,
};
