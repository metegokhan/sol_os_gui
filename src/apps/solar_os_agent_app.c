#include "solar_os_agent_app.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_agent.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_queue.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"
#include "solar_os_terminal.h"
#include "solar_os_wifi.h"

#define AGENT_APP_TASK_STACK 16384U
#define AGENT_APP_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define AGENT_APP_EVENT_QUEUE_LEN 16U

SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(AGENT_APP_TASK_STACK);

typedef struct {
    QueueHandle_t events;
    TaskHandle_t task;
    char *prompt;
    volatile bool task_done;
    volatile bool stopping;
    bool running;
    bool text_started;
} agent_app_state_t;

static const char *TAG = "agent_app";
static agent_app_state_t agent_app;
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
    const solar_os_agent_request_t request = {
        .prompt = agent_app.prompt,
        .event_handler = agent_app_service_event,
        .user_data = &agent_app,
    };
    (void)solar_os_agent_run(&request);
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
    (void)solar_os_agent_init();

    const int argc = solar_os_context_argc(ctx);
    if (argc < 3 || strcmp(solar_os_context_argv(ctx, 1), "ask") != 0) {
        solar_os_shell_io_writeln(agent_app_io(ctx),
                                  "agent: launch with agent ask PROMPT...");
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_return_to_shell(ctx);
        return ESP_OK;
    }

    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (!wifi.started || !wifi.connected || !wifi.has_ip) {
        solar_os_shell_io_writeln(agent_app_io(ctx),
                                  "agent: Wi-Fi is not connected");
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_return_to_shell(ctx);
        return ESP_OK;
    }

    solar_os_agent_status_t status;
    (void)solar_os_agent_get_status(&status);
    if (!status.configured) {
        solar_os_shell_io_writeln(agent_app_io(ctx),
                                  "agent: configure endpoint and model first");
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_return_to_shell(ctx);
        return ESP_OK;
    }

    esp_err_t err = agent_app_build_prompt(ctx);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(agent_app_io(ctx),
                                 "agent: invalid prompt: %s\n",
                                 esp_err_to_name(err));
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_return_to_shell(ctx);
        return ESP_OK;
    }
    agent_app.events = solar_os_queue_create(AGENT_APP_EVENT_QUEUE_LEN,
                                              sizeof(solar_os_agent_event_t));
    if (agent_app.events == NULL) {
        solar_os_shell_io_writeln(agent_app_io(ctx), "agent: out of memory");
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_cleanup();
        agent_app_return_to_shell(ctx);
        return ESP_OK;
    }

    solar_os_shell_io_printf_bold(agent_app_io(ctx),
                                  "agent (%s)\n",
                                  status.model);
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
        (void)solar_os_agent_cancel();
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
        agent_app_drain_events(ctx);
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
            (void)solar_os_agent_cancel();
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
