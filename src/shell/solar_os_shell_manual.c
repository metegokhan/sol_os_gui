#include "solar_os_shell_commands.h"

#include <stdio.h>
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_DOCS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_docs.h"
#include "solar_os_task.h"
#endif
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_APP_LESS
#include "solar_os_less.h"
#endif
#include "solar_os_manual.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"

#define MAN_QUERY_MAX 160U
#define MAN_SEARCH_MAX 12U
#define DOCS_UPDATE_TASK_STACK 16384U
#define DOCS_UPDATE_WAIT_MS 100U

#if SOLAR_OS_PACKAGE_SERVICE_DOCS
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(DOCS_UPDATE_TASK_STACK);
#endif

static void man_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  man TOPIC");
    solar_os_shell_io_writeln(io, "  man -k QUERY...");
    solar_os_shell_io_writeln(io, "  man --list");
}

static bool man_join_args(int argc,
                          char **argv,
                          int first,
                          char *buffer,
                          size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0U || first >= argc) {
        return false;
    }
    buffer[0] = '\0';
    size_t used = 0U;
    for (int i = first; i < argc; i++) {
        const char *arg = argv[i];
        const size_t arg_len = arg != NULL ? strlen(arg) : 0U;
        const size_t separator = used > 0U ? 1U : 0U;
        if (arg_len == 0U || used + separator + arg_len >= buffer_len) {
            return false;
        }
        if (separator != 0U) {
            buffer[used++] = ' ';
        }
        memcpy(buffer + used, arg, arg_len);
        used += arg_len;
        buffer[used] = '\0';
    }
    return used > 0U;
}

static void man_list(solar_os_shell_io_t *io)
{
    for (size_t i = 0U; i < solar_os_manual_count(); i++) {
        const solar_os_manual_page_t *page = solar_os_manual_get(i);
        if (page != NULL) {
            solar_os_shell_io_printf(io,
                                     "%-18s %-8s %s\n",
                                     page->id,
                                     page->section,
                                     page->summary);
        }
    }
}

static size_t man_search(solar_os_shell_io_t *io, const char *query)
{
    const solar_os_manual_page_t *matches[MAN_SEARCH_MAX] = {0};
    const size_t count =
        solar_os_manual_search(query, matches, MAN_SEARCH_MAX);
    if (count == 0U) {
        solar_os_shell_io_printf(io, "man: no entries for %s\n", query);
        return 0U;
    }
    for (size_t i = 0U; i < count; i++) {
        solar_os_shell_io_printf(io,
                                 "%-18s - %s\n",
                                 matches[i]->id,
                                 matches[i]->summary);
    }
    return count;
}

static void man_print_page(solar_os_shell_io_t *io,
                           const solar_os_manual_page_t *page)
{
    const char *body = NULL;
    size_t len = 0U;
    bool owned = false;
    if (page == NULL ||
        solar_os_manual_load_body(page, &body, &len, &owned) != ESP_OK) {
        return;
    }
    solar_os_shell_io_write(io, body);
    if (len == 0U || body[len - 1U] != '\n') {
        solar_os_shell_io_newline(io);
    }
    solar_os_manual_release_text(body, owned);
}

static bool man_launch_pager(solar_os_context_t *ctx,
                             solar_os_shell_io_t *io,
                             const solar_os_manual_page_t *page)
{
#if SOLAR_OS_PACKAGE_APP_LESS
    char source[SOLAR_OS_APP_ARG_LEN];
    const int written = snprintf(source, sizeof(source), "man:%s", page->id);
    if (written < 0 || (size_t)written >= sizeof(source)) {
        return false;
    }
    char *launch_argv[] = {"less", source};
    const esp_err_t err =
        solar_os_context_request_launch(ctx, &solar_os_less_app, 2, launch_argv);
    if (err == ESP_OK) {
        solar_os_shell_session_prepare_foreground_launch(ctx, false);
        return true;
    }
    solar_os_shell_io_printf(io,
                             "man: pager unavailable: %s\n",
                             esp_err_to_name(err));
#else
    (void)ctx;
    (void)io;
    (void)page;
#endif
    return false;
}

void solar_os_shell_cmd_man(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        man_list(io);
        return;
    }
    if (argc >= 3 && (strcmp(argv[1], "-k") == 0 ||
                      strcmp(argv[1], "--apropos") == 0)) {
        char query[MAN_QUERY_MAX];
        if (!man_join_args(argc, argv, 2, query, sizeof(query))) {
            man_usage(io);
            return;
        }
        (void)man_search(io, query);
        return;
    }
    if (argc != 2) {
        man_usage(io);
        return;
    }

    const solar_os_manual_page_t *page = solar_os_manual_find(argv[1]);
    if (page == NULL) {
        (void)man_search(io, argv[1]);
        return;
    }
    if (!man_launch_pager(ctx, io, page)) {
        man_print_page(io, page);
    }
}

#if SOLAR_OS_PACKAGE_SERVICE_DOCS
typedef struct {
    esp_err_t result;
    volatile bool done;
} docs_update_worker_t;

static void docs_update_task(void *arg)
{
    docs_update_worker_t *worker = (docs_update_worker_t *)arg;
    if (worker != NULL) {
        worker->result = solar_os_docs_update();
        worker->done = true;
    }
    solar_os_task_delete_internal(NULL);
}

static esp_err_t docs_run_update(void)
{
    docs_update_worker_t worker = {
        .result = ESP_FAIL,
    };
    TaskHandle_t task = NULL;
    if (solar_os_task_create_pinned_internal(docs_update_task,
                                             "docs_update",
                                             DOCS_UPDATE_TASK_STACK,
                                             &worker,
                                             tskIDLE_PRIORITY + 1U,
                                             &task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    TickType_t delay = pdMS_TO_TICKS(DOCS_UPDATE_WAIT_MS);
    if (delay == 0U) {
        delay = 1U;
    }
    while (!worker.done) {
        vTaskDelay(delay);
    }
    return worker.result;
}

static void docs_print_status(solar_os_shell_io_t *io)
{
    solar_os_docs_status_t status;
    const esp_err_t err = solar_os_docs_get_status(&status);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "docs: status failed: %s\n", esp_err_to_name(err));
        return;
    }
    solar_os_shell_io_printf(io, "Firmware: %s\n", status.version);
    solar_os_shell_io_printf(io,
                             "External manual: %s\n",
                             status.available ? "active" : "embedded fallback");
    if (status.available) {
        solar_os_shell_io_printf(io, "Revision: %s\n", status.revision);
        solar_os_shell_io_printf(io, "Pages: %u\n", (unsigned)status.page_count);
    }
    solar_os_shell_io_printf(io, "Updating: %s\n", status.updating ? "yes" : "no");
    if (status.last_error[0] != '\0') {
        solar_os_shell_io_printf(io, "Last error: %s\n", status.last_error);
    }
}

void solar_os_shell_cmd_docs(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        docs_print_status(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "update") == 0) {
        solar_os_shell_io_writeln(io, "docs: downloading signed manual");
        solar_os_shell_io_flush(io);
        const esp_err_t err = docs_run_update();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(io, "docs: manual updated");
            docs_print_status(io);
        } else {
            solar_os_shell_io_printf(io,
                                     "docs: update failed: %s\n",
                                     esp_err_to_name(err));
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        const esp_err_t err = solar_os_docs_reset();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(io, "docs: using embedded manual");
        } else {
            solar_os_shell_io_printf(io,
                                     "docs: reset failed: %s\n",
                                     esp_err_to_name(err));
        }
        return;
    }
    solar_os_shell_io_writeln(io, "usage: docs status|update|reset");
}
#endif
