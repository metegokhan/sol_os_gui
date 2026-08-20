#include "solar_os_go.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_appbar.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"

#define GO_BOARD_SIZE 9
#define GO_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(GO_STACK_SIZE);

/* 0=Empty, 1=Black, 2=White */
typedef struct {
    uint8_t board[GO_BOARD_SIZE][GO_BOARD_SIZE];
    int cursor_r;
    int cursor_c;
    bool black_turn;
    uint32_t captured_black;
    uint32_t captured_white;
    char status_msg[64];
} go_state_t;

static void *go_state_ptr;
#define gostate (*(go_state_t *)go_state_ptr)

static void go_init_board(void)
{
    memset(&gostate, 0, sizeof(gostate));
    gostate.cursor_r = 4;
    gostate.cursor_c = 4;
    gostate.black_turn = true;
    strlcpy(gostate.status_msg, "Black's turn to play", sizeof(gostate.status_msg));
}

static bool count_liberties_rec(int r, int c, uint8_t color, bool visited[GO_BOARD_SIZE][GO_BOARD_SIZE], int *liberties)
{
    if (r < 0 || r >= GO_BOARD_SIZE || c < 0 || c >= GO_BOARD_SIZE) return true;
    if (visited[r][c]) return true;
    visited[r][c] = true;

    if (gostate.board[r][c] == 0) {
        (*liberties)++;
        return true;
    }
    if (gostate.board[r][c] != color) {
        return true;
    }

    count_liberties_rec(r - 1, c, color, visited, liberties);
    count_liberties_rec(r + 1, c, color, visited, liberties);
    count_liberties_rec(r, c - 1, color, visited, liberties);
    count_liberties_rec(r, c + 1, color, visited, liberties);
    return true;
}

static void remove_group_rec(int r, int c, uint8_t color, uint32_t *count)
{
    if (r < 0 || r >= GO_BOARD_SIZE || c < 0 || c >= GO_BOARD_SIZE) return;
    if (gostate.board[r][c] != color) return;

    gostate.board[r][c] = 0;
    (*count)++;

    remove_group_rec(r - 1, c, color, count);
    remove_group_rec(r + 1, c, color, count);
    remove_group_rec(r, c - 1, color, count);
    remove_group_rec(r, c + 1, color, count);
}

static uint32_t check_and_capture(int r, int c, uint8_t opp_color)
{
    if (r < 0 || r >= GO_BOARD_SIZE || c < 0 || c >= GO_BOARD_SIZE) return 0;
    if (gostate.board[r][c] != opp_color) return 0;

    bool visited[GO_BOARD_SIZE][GO_BOARD_SIZE] = {false};
    int liberties = 0;
    count_liberties_rec(r, c, opp_color, visited, &liberties);

    if (liberties == 0) {
        uint32_t removed = 0;
        remove_group_rec(r, c, opp_color, &removed);
        return removed;
    }
    return 0;
}

static void go_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header */
    char turn_hdr[32];
    snprintf(turn_hdr, sizeof(turn_hdr), "Turn: %s", gostate.black_turn ? "Black" : "White");
    solar_os_appbar_header_t header = {0};
    header.title = "Go (9x9)";
    header.show_back = true;
    header.status_line = turn_hdr;
    solar_os_appbar_draw_header(gfx, &header);

    /* 2. 9x9 Grid (X: 24..240, spacing: 26px) */
    const int gx = 28;
    const int gy = solar_os_appbar_header_height(gfx) + solar_os_appbar_status_line_height(gfx) + 4;
    const int sp = 26;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, gx - 4, gy - 4, 8 * sp + 8, 8 * sp + 8);

    /* Grid Lines */
    for (int i = 0; i < GO_BOARD_SIZE; i++) {
        solar_os_gfx_line(gfx, gx, gy + i * sp, gx + 8 * sp, gy + i * sp);
        solar_os_gfx_line(gfx, gx + i * sp, gy, gx + i * sp, gy + 8 * sp);
    }

    /* Star points (Hoshi) for 9x9: (2,2), (6,2), (4,4), (2,6), (6,6) */
    static const int hoshi[5][2] = {{2,2}, {6,2}, {4,4}, {2,6}, {6,6}};
    for (int i = 0; i < 5; i++) {
        solar_os_gfx_fill_circle(gfx, gx + hoshi[i][0] * sp, gy + hoshi[i][1] * sp, 2);
    }

    /* Stones */
    for (int r = 0; r < GO_BOARD_SIZE; r++) {
        for (int c = 0; c < GO_BOARD_SIZE; c++) {
            const int sx = gx + c * sp;
            const int sy = gy + r * sp;
            const uint8_t color = gostate.board[r][c];

            if (color == 1) { /* Black Stone */
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_circle(gfx, sx, sy, 10);
            } else if (color == 2) { /* White Stone */
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
                solar_os_gfx_fill_circle(gfx, sx, sy, 10);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_circle(gfx, sx, sy, 10);
                solar_os_gfx_circle(gfx, sx, sy, 9);
            }

            if (r == gostate.cursor_r && c == gostate.cursor_c) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_circle(gfx, sx, sy, 13);
                solar_os_gfx_line(gfx, sx - 16, sy, sx - 13, sy);
                solar_os_gfx_line(gfx, sx + 13, sy, sx + 16, sy);
                solar_os_gfx_line(gfx, sx, sy - 16, sx, sy - 13);
                solar_os_gfx_line(gfx, sx, sy + 13, sx, sy + 16);
            }
        }
    }

    /* 3. Right Sidebar: Status (the CONTROLS list that used to live here
     * duplicated the footer's shortcuts verbatim, and isn't needed now
     * that the board itself is tappable). */
    const int sidebar_h = screen_h - solar_os_appbar_footer_height(gfx) - gy - 4;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 260, gy, screen_w - 268, sidebar_h);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 270, gy + 22, "GO MATCH STATS");
    solar_os_gfx_line(gfx, 264, gy + 28, screen_w - 12, gy + 28);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 270, gy + 49, gostate.status_msg);

    char cap_str1[32], cap_str2[32];
    snprintf(cap_str1, sizeof(cap_str1), "Black Captures: %u", (unsigned)gostate.captured_white);
    snprintf(cap_str2, sizeof(cap_str2), "White Captures: %u", (unsigned)gostate.captured_black);
    solar_os_gfx_text(gfx, 270, gy + 79, cap_str1);
    solar_os_gfx_text(gfx, 270, gy + 99, cap_str2);

    /* 4. Footer */
    solar_os_appbar_shortcut_t footer_items[2];
    footer_items[0] = (solar_os_appbar_shortcut_t){ .key = 'p', .ctrl = false, .label = "Pass" };
    footer_items[1] = (solar_os_appbar_shortcut_t){ .key = 'r', .ctrl = false, .label = "Reset" };
    const solar_os_appbar_shortcuts_t footer_shortcuts = { .items = footer_items, .count = 2 };
    solar_os_appbar_draw_footer(gfx, &footer_shortcuts);

    solar_os_gfx_present(gfx);
}

static esp_err_t go_start(solar_os_context_t *ctx)
{
    go_init_board();
    solar_os_context_set_graphics_active(ctx, true);
    go_render(ctx);
    return ESP_OK;
}

static void go_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

/* Places a stone at (r, c) if it's empty, checks captures, and passes the
 * turn -- shared by the Enter-key handler and the click handler's board
 * tap path. */
static void go_place_stone(solar_os_context_t *ctx, int r, int c)
{
    if (gostate.board[r][c] != 0) return;

    const uint8_t color = gostate.black_turn ? 1 : 2;
    const uint8_t opp = gostate.black_turn ? 2 : 1;

    gostate.board[r][c] = color;

    uint32_t caps = 0;
    caps += check_and_capture(r - 1, c, opp);
    caps += check_and_capture(r + 1, c, opp);
    caps += check_and_capture(r, c - 1, opp);
    caps += check_and_capture(r, c + 1, opp);

    if (gostate.black_turn) gostate.captured_white += caps;
    else gostate.captured_black += caps;

    gostate.black_turn = !gostate.black_turn;
    snprintf(gostate.status_msg, sizeof(gostate.status_msg), "%s's turn to play", gostate.black_turn ? "Black" : "White");
    go_render(ctx);
}

static bool go_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        solar_os_appbar_header_t header = {0};
        header.show_back = true;
        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, px, py, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                solar_os_context_request_exit(ctx);
            }
            return true;
        }

        solar_os_appbar_shortcut_t footer_items[2];
        footer_items[0] = (solar_os_appbar_shortcut_t){ .key = 'p', .ctrl = false, .label = "Pass" };
        footer_items[1] = (solar_os_appbar_shortcut_t){ .key = 'r', .ctrl = false, .label = "Reset" };
        const solar_os_appbar_shortcuts_t footer_shortcuts = { .items = footer_items, .count = 2 };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &footer_shortcuts, px, py, &fhit) &&
            fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM) {
            if (fhit.index == 0) {
                gostate.black_turn = !gostate.black_turn;
                snprintf(gostate.status_msg, sizeof(gostate.status_msg), "%s passed", gostate.black_turn ? "White" : "Black");
            } else {
                go_init_board();
            }
            go_render(ctx);
            return true;
        }

        /* Mirrors go_render()'s 9x9 grid geometry. */
        const int gx = 28;
        const int gy = solar_os_appbar_header_height(gfx) + solar_os_appbar_status_line_height(gfx) + 4;
        const int sp = 26;
        const int c = (int)((px - gx + sp / 2) / sp);
        const int r = (int)((py - gy + sp / 2) / sp);
        if (r >= 0 && r < GO_BOARD_SIZE && c >= 0 && c < GO_BOARD_SIZE) {
            gostate.cursor_r = r;
            gostate.cursor_c = c;
            go_place_stone(ctx, r, c);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W' || ch == 'k' || ch == 'K') {
            if (gostate.cursor_r > 0) gostate.cursor_r--;
            go_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S' || ch == 'j' || ch == 'J') {
            if (gostate.cursor_r < GO_BOARD_SIZE - 1) gostate.cursor_r++;
            go_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A' || ch == 'h' || ch == 'H') {
            if (gostate.cursor_c > 0) gostate.cursor_c--;
            go_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D' || ch == 'l' || ch == 'L') {
            if (gostate.cursor_c < GO_BOARD_SIZE - 1) gostate.cursor_c++;
            go_render(ctx);
            return true;
        }

        if (ch == '\r' || ch == '\n' || ch == ' ') {
            go_place_stone(ctx, gostate.cursor_r, gostate.cursor_c);
            return true;
        }

        if (ch == 'p' || ch == 'P') {
            gostate.black_turn = !gostate.black_turn;
            snprintf(gostate.status_msg, sizeof(gostate.status_msg), "%s passed", gostate.black_turn ? "White" : "Black");
            go_render(ctx);
            return true;
        }

        if (ch == 'r' || ch == 'R') {
            go_init_board();
            go_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_go_app = {
    .name = "go",
    .summary = "classic 9x9 board game of Go",
    .flags = 0,
    .start = go_start,
    .stop = go_stop,
    .event = go_event,
    .state_slot = &go_state_ptr,
    .state_size = sizeof(go_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = GO_STACK_SIZE,
};
