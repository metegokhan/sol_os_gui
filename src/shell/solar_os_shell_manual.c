#include "solar_os_shell_commands.h"

#include <stdio.h>
#include <string.h>

#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_APP_LESS
#include "solar_os_less.h"
#endif
#include "solar_os_manual.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"

#define MAN_QUERY_MAX 160U
#define MAN_SEARCH_MAX 12U

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
    if (page == NULL || page->body == NULL) {
        return;
    }
    solar_os_shell_io_write(io, page->body);
    const size_t len = strlen(page->body);
    if (len == 0U || page->body[len - 1U] != '\n') {
        solar_os_shell_io_newline(io);
    }
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
