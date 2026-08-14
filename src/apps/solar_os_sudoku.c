#include "solar_os_sudoku.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"

#define SUDOKU_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(SUDOKU_STACK_SIZE);

typedef struct {
    uint8_t grid[9][9];
    bool initial[9][9];
    int cursor_r;
    int cursor_c;
    int puzzle_idx;
    bool solved;
    char status_msg[64];
} sudoku_state_t;

static void *sudoku_state_ptr;
#define sudoku (*(sudoku_state_t *)sudoku_state_ptr)

static const uint8_t preset_puzzles[3][9][9] = {
    { /* Easy */
        {5,3,0,0,7,0,0,0,0},
        {6,0,0,1,9,5,0,0,0},
        {0,9,8,0,0,0,0,6,0},
        {8,0,0,0,6,0,0,0,3},
        {4,0,0,8,0,3,0,0,1},
        {7,0,0,0,2,0,0,0,6},
        {0,6,0,0,0,0,2,8,0},
        {0,0,0,4,1,9,0,0,5},
        {0,0,0,0,8,0,0,7,9}
    },
    { /* Medium */
        {0,0,0,2,6,0,7,0,1},
        {6,8,0,0,7,0,0,9,0},
        {1,9,0,0,0,4,5,0,0},
        {8,2,0,1,0,0,0,4,0},
        {0,0,4,6,0,2,9,0,0},
        {0,5,0,0,0,3,0,2,8},
        {0,0,9,3,0,0,0,7,4},
        {0,4,0,0,5,0,0,3,6},
        {7,0,3,0,1,8,0,0,0}
    },
    { /* Hard */
        {1,0,0,0,0,7,0,9,0},
        {0,3,0,0,2,0,0,0,8},
        {0,0,9,6,0,0,5,0,0},
        {0,0,5,3,0,0,9,0,0},
        {0,1,0,0,8,0,0,0,2},
        {6,0,0,0,0,4,0,0,0},
        {3,0,0,0,0,0,0,1,0},
        {0,4,0,0,0,0,0,0,7},
        {0,0,7,0,0,0,3,0,0}
    }
};

static void sudoku_load_puzzle(int idx)
{
    sudoku.puzzle_idx = idx % 3;
    sudoku.cursor_r = 4;
    sudoku.cursor_c = 4;
    sudoku.solved = false;
    strlcpy(sudoku.status_msg, "Enter 1-9 in empty cells", sizeof(sudoku.status_msg));

    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            uint8_t val = preset_puzzles[sudoku.puzzle_idx][r][c];
            sudoku.grid[r][c] = val;
            sudoku.initial[r][c] = (val != 0);
        }
    }
}

static bool sudoku_check_win(void)
{
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            if (sudoku.grid[r][c] == 0) return false;
        }
    }
    return true;
}

static void sudoku_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 16, "SOLAR SUDOKU");

    static const char *diffs[] = {"EASY", "MEDIUM", "HARD"};
    char diff_hdr[32];
    snprintf(diff_hdr, sizeof(diff_hdr), "DIFFICULTY: %s", diffs[sudoku.puzzle_idx]);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t dh_w = solar_os_gfx_text_width(gfx, diff_hdr);
    solar_os_gfx_text(gfx, screen_w - (int)dh_w - 8, 16, diff_hdr);

    /* 2. 9x9 Grid (X: 20..245, Y: 34..259, cell: 25px) */
    const int gx = 22;
    const int gy = 34;
    const int cell = 25;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, gx, gy, 9 * cell, 9 * cell);

    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            const int cx = gx + c * cell;
            const int cy = gy + r * cell;
            const bool is_cursor = (r == sudoku.cursor_r && c == sudoku.cursor_c);
            const uint8_t val = sudoku.grid[r][c];
            const bool is_init = sudoku.initial[r][c];

            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, cx, cy, cell, cell);

            if (is_cursor) {
                solar_os_gfx_rect(gfx, cx + 1, cy + 1, cell - 2, cell - 2);
                solar_os_gfx_rect(gfx, cx + 2, cy + 2, cell - 4, cell - 4);
            }

            if (val != 0) {
                char num_str[4];
                snprintf(num_str, sizeof(num_str), "%d", val);
                solar_os_gfx_set_font(gfx, is_init ? SOLAR_OS_GFX_FONT_BOLD : SOLAR_OS_GFX_FONT_SMALL);
                solar_os_gfx_text(gfx, cx + 8, cy + 17, num_str);
            }
        }
    }

    /* Thick 3x3 block borders */
    for (int b = 0; b <= 3; b++) {
        solar_os_gfx_line(gfx, gx + b * 3 * cell, gy, gx + b * 3 * cell, gy + 9 * cell);
        solar_os_gfx_line(gfx, gx + b * 3 * cell + 1, gy, gx + b * 3 * cell + 1, gy + 9 * cell);
        solar_os_gfx_line(gfx, gx, gy + b * 3 * cell, gx + 9 * cell, gy + b * 3 * cell);
        solar_os_gfx_line(gfx, gx, gy + b * 3 * cell + 1, gx + 9 * cell, gy + b * 3 * cell + 1);
    }

    /* 3. Right Sidebar */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 260, 34, screen_w - 268, 226);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 270, 56, "PUZZLE INFO");
    solar_os_gfx_line(gfx, 264, 62, screen_w - 12, 62);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 270, 85, sudoku.status_msg);

    if (sudoku.solved) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 270, 115, "CONGRATULATIONS!");
        solar_os_gfx_text(gfx, 270, 135, "PUZZLE SOLVED!");
    }

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 270, 170, "CONTROLS:");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 270, 190, "[1-9] Enter Number");
    solar_os_gfx_text(gfx, 270, 210, "[0/BS] Clear Cell");
    solar_os_gfx_text(gfx, 270, 230, "[N] Next Difficulty");

    /* 4. Footer */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 293, "[ARROWS] Cursor | [1-9] Place | [0/BS] Clear | [N] New Puzzle | [ESC] Exit");

    solar_os_gfx_present(gfx);
}

static esp_err_t sudoku_start(solar_os_context_t *ctx)
{
    sudoku_load_puzzle(0);
    solar_os_context_set_graphics_active(ctx, true);
    sudoku_render(ctx);
    return ESP_OK;
}

static void sudoku_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool sudoku_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W' || ch == 'k' || ch == 'K') {
            if (sudoku.cursor_r > 0) sudoku.cursor_r--;
            sudoku_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S' || ch == 'j' || ch == 'J') {
            if (sudoku.cursor_r < 8) sudoku.cursor_r++;
            sudoku_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A' || ch == 'h' || ch == 'H') {
            if (sudoku.cursor_c > 0) sudoku.cursor_c--;
            sudoku_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D' || ch == 'l' || ch == 'L') {
            if (sudoku.cursor_c < 8) sudoku.cursor_c++;
            sudoku_render(ctx);
            return true;
        }

        if (ch >= '1' && ch <= '9') {
            const int r = sudoku.cursor_r;
            const int c = sudoku.cursor_c;
            if (!sudoku.initial[r][c]) {
                sudoku.grid[r][c] = (uint8_t)(ch - '0');
                if (sudoku_check_win()) {
                    sudoku.solved = true;
                    strlcpy(sudoku.status_msg, "Puzzle completed!", sizeof(sudoku.status_msg));
                }
                sudoku_render(ctx);
            }
            return true;
        }

        if (ch == '0' || ch == '\b' || ch == 127 || ch == 'c' || ch == 'C') {
            const int r = sudoku.cursor_r;
            const int c = sudoku.cursor_c;
            if (!sudoku.initial[r][c]) {
                sudoku.grid[r][c] = 0;
                sudoku.solved = false;
                sudoku_render(ctx);
            }
            return true;
        }

        if (ch == 'n' || ch == 'N') {
            sudoku_load_puzzle((sudoku.puzzle_idx + 1) % 3);
            sudoku_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_sudoku_app = {
    .name = "sudoku",
    .summary = "classic 9x9 Sudoku number puzzle",
    .flags = 0,
    .start = sudoku_start,
    .stop = sudoku_stop,
    .event = sudoku_event,
    .state_slot = &sudoku_state_ptr,
    .state_size = sizeof(sudoku_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = SUDOKU_STACK_SIZE,
};
