#include "solar_os_docs_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
#include "solar_os_docs.h"
#endif
#include "solar_os_keys.h"
#include "solar_os_less.h"
#include "solar_os_manual.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"

#define DOCS_APP_LINE_MAX (SOLAR_OS_TERMINAL_MAX_COLS * 4U + 1U)

typedef struct {
    solar_os_tui_t tui;
    size_t cursor;
    size_t top;
} docs_app_state_t;

static docs_app_state_t docs_app;

static void docs_app_write_cell(size_t row,
                                size_t col,
                                size_t width,
                                const char *text,
                                uint8_t attr)
{
    const size_t rows = solar_os_tui_rows(&docs_app.tui);
    const size_t cols = solar_os_tui_cols(&docs_app.tui);
    char clipped[DOCS_APP_LINE_MAX];

    if (row >= rows || col >= cols || width == 0U) {
        return;
    }
    if (col + width > cols) {
        width = cols - col;
    }
    solar_os_tui_fill(&docs_app.tui, row, col, 1U, width, ' ', attr);

    size_t copy = text != NULL ? strlen(text) : 0U;
    if (copy > width) {
        copy = width;
    }
    if (copy > sizeof(clipped) - 1U) {
        copy = sizeof(clipped) - 1U;
    }
    if (copy > 0U) {
        memcpy(clipped, text, copy);
    }
    clipped[copy] = '\0';
    if (copy > 0U) {
        solar_os_tui_addstr(&docs_app.tui, row, col, clipped, attr);
    }
}

static size_t docs_app_visible_rows(void)
{
    const size_t rows = solar_os_tui_rows(&docs_app.tui);
    return rows > 2U ? rows - 2U : 0U;
}

static void docs_app_ensure_visible(void)
{
    const size_t count = solar_os_manual_count();
    const size_t visible = docs_app_visible_rows();
    if (count == 0U || visible == 0U) {
        docs_app.cursor = 0U;
        docs_app.top = 0U;
        return;
    }
    if (docs_app.cursor >= count) {
        docs_app.cursor = count - 1U;
    }
    if (docs_app.cursor < docs_app.top) {
        docs_app.top = docs_app.cursor;
    } else if (docs_app.cursor >= docs_app.top + visible) {
        docs_app.top = docs_app.cursor - visible + 1U;
    }
}

static void docs_app_header(char *line, size_t line_len)
{
    const size_t count = solar_os_manual_count();
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
    solar_os_docs_status_t status;
    if (solar_os_docs_get_status(&status) == ESP_OK && status.available) {
        snprintf(line,
                 line_len,
                 "SolarOS docs  %u topics  downloaded %s",
                 (unsigned)count,
                 status.revision);
        return;
    }
#endif
    snprintf(line,
             line_len,
             "SolarOS docs  %u topics  embedded",
             (unsigned)count);
}

static void docs_app_render(void)
{
    const size_t rows = solar_os_tui_rows(&docs_app.tui);
    const size_t cols = solar_os_tui_cols(&docs_app.tui);
    const size_t visible = docs_app_visible_rows();
    char line[DOCS_APP_LINE_MAX];

    docs_app_ensure_visible();
    solar_os_tui_set_cursor_visible(&docs_app.tui, false);
    solar_os_tui_clear(&docs_app.tui);
    docs_app_header(line, sizeof(line));
    docs_app_write_cell(0U,
                        0U,
                        cols,
                        line,
                        SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);

    if (rows < 3U || cols < 12U) {
        docs_app_write_cell(1U,
                            0U,
                            cols,
                            "terminal too small",
                            SOLAR_OS_TUI_ATTR_NORMAL);
    } else {
        for (size_t row = 0U; row < visible; row++) {
            const size_t index = docs_app.top + row;
            const solar_os_manual_page_t *page = solar_os_manual_get(index);
            if (page == NULL) {
                docs_app_write_cell(row + 1U,
                                    0U,
                                    cols,
                                    "",
                                    SOLAR_OS_TUI_ATTR_NORMAL);
                continue;
            }
            if (cols >= 56U) {
                snprintf(line,
                         sizeof(line),
                         "%-22.22s %-9.9s %s",
                         page->title,
                         page->section,
                         page->summary);
            } else {
                snprintf(line,
                         sizeof(line),
                         "%-22.22s %s",
                         page->title,
                         page->summary);
            }
            const uint8_t attr = index == docs_app.cursor ?
                SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD :
                SOLAR_OS_TUI_ATTR_NORMAL;
            docs_app_write_cell(row + 1U, 0U, cols, line, attr);
        }
    }

    if (rows > 0U) {
        docs_app_write_cell(rows - 1U,
                            0U,
                            cols,
                            "Enter open  arrows move  PgUp/PgDn page  q quit",
                            SOLAR_OS_TUI_ATTR_INVERSE);
    }
    solar_os_tui_refresh(&docs_app.tui);
}

static bool docs_app_open_selected(solar_os_context_t *ctx)
{
    const solar_os_manual_page_t *page =
        solar_os_manual_get(docs_app.cursor);
    if (page == NULL) {
        return false;
    }
    char app_arg[SOLAR_OS_APP_ARG_LEN];
    char source_arg[SOLAR_OS_APP_ARG_LEN];
    char *argv[] = {app_arg, source_arg};
    strlcpy(app_arg, "less", sizeof(app_arg));
    const int written =
        snprintf(source_arg, sizeof(source_arg), "man:%s", page->id);
    if (written < 0 || (size_t)written >= sizeof(source_arg)) {
        return false;
    }
    return solar_os_context_request_launch_ex(ctx,
                                              &solar_os_less_app,
                                              2,
                                              argv,
                                              SOLAR_OS_LAUNCH_CHILD_RETURN) == ESP_OK;
}

static void docs_app_move(int delta)
{
    const size_t count = solar_os_manual_count();
    if (count == 0U) {
        return;
    }
    if (delta < 0 && docs_app.cursor > 0U) {
        docs_app.cursor--;
    } else if (delta > 0 && docs_app.cursor + 1U < count) {
        docs_app.cursor++;
    }
}

static void docs_app_page(bool down)
{
    const size_t count = solar_os_manual_count();
    const size_t visible = docs_app_visible_rows();
    const size_t step = visible > 1U ? visible - 1U : 1U;
    if (count == 0U) {
        return;
    }
    if (down) {
        docs_app.cursor = docs_app.cursor + step < count ?
            docs_app.cursor + step : count - 1U;
    } else {
        docs_app.cursor = docs_app.cursor > step ?
            docs_app.cursor - step : 0U;
    }
}

static esp_err_t docs_app_start(solar_os_context_t *ctx)
{
    memset(&docs_app, 0, sizeof(docs_app));
    const esp_err_t err = solar_os_tui_begin(&docs_app.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    (void)solar_os_tui_enable_diff(&docs_app.tui, true);

    const char *requested = solar_os_context_argc(ctx) >= 2 ?
        solar_os_context_argv(ctx, 1) : NULL;
    const solar_os_manual_page_t *page =
        requested != NULL ? solar_os_manual_find(requested) : NULL;
    if (page != NULL) {
        for (size_t i = 0U; i < solar_os_manual_count(); i++) {
            if (solar_os_manual_get(i) == page) {
                docs_app.cursor = i;
                break;
            }
        }
    }
    docs_app_render();
    return ESP_OK;
}

static void docs_app_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&docs_app.tui, true);
    solar_os_tui_refresh(&docs_app.tui);
    solar_os_tui_end(&docs_app.tui);
    memset(&docs_app, 0, sizeof(docs_app));
}

static void docs_app_resume(solar_os_context_t *ctx)
{
    (void)ctx;
    docs_app_render();
}

static void docs_app_title(solar_os_context_t *ctx,
                           char *buffer,
                           size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len > 0U) {
        strlcpy(buffer, "docs", buffer_len);
    }
}

static bool docs_app_event(solar_os_context_t *ctx,
                           const solar_os_event_t *event)
{
    if (event == NULL || event->type != SOLAR_OS_EVENT_CHAR) {
        return true;
    }
    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE ||
        ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }

    switch (ch) {
    case SOLAR_OS_KEY_UP:
    case 'k':
        docs_app_move(-1);
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        docs_app_move(1);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        docs_app_page(false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        docs_app_page(true);
        break;
    case SOLAR_OS_KEY_HOME:
        docs_app.cursor = 0U;
        break;
    case SOLAR_OS_KEY_END:
        if (solar_os_manual_count() > 0U) {
            docs_app.cursor = solar_os_manual_count() - 1U;
        }
        break;
    case '\r':
    case '\n':
    case SOLAR_OS_KEY_RIGHT:
        (void)docs_app_open_selected(ctx);
        return true;
    default:
        return true;
    }
    docs_app_render();
    return true;
}

const solar_os_app_t solar_os_docs_app = {
    .name = "docs",
    .summary = "browse the SolarOS manual",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = docs_app_start,
    .resume = docs_app_resume,
    .stop = docs_app_stop,
    .event = docs_app_event,
    .title = docs_app_title,
};
