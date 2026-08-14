#include "solar_os_chess.h"

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

#define CHESS_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(CHESS_STACK_SIZE);

/* Pieces: 0=Empty, 1=P, 2=N, 3=B, 4=R, 5=Q, 6=K. Positive=White, Negative=Black */
typedef struct {
    int8_t board[8][8];
    int cursor_r;
    int cursor_c;
    int sel_r;
    int sel_c;
    bool selected;
    bool white_turn;
    char status_msg[64];
    uint32_t move_count;
} chess_state_t;

static void *chess_state_ptr;
#define chess (*(chess_state_t *)chess_state_ptr)

static void chess_init_board(void)
{
    memset(&chess, 0, sizeof(chess));
    chess.cursor_r = 6;
    chess.cursor_c = 4;
    chess.sel_r = -1;
    chess.sel_c = -1;
    chess.white_turn = true;
    chess.move_count = 1;
    strlcpy(chess.status_msg, "White's turn to move", sizeof(chess.status_msg));

    static const int8_t init_row[8] = {4, 2, 3, 5, 6, 3, 2, 4};
    for (int c = 0; c < 8; c++) {
        chess.board[0][c] = -init_row[c]; /* Black back row */
        chess.board[1][c] = -1;           /* Black pawns */
        chess.board[6][c] = 1;            /* White pawns */
        chess.board[7][c] = init_row[c];  /* White back row */
    }
}

static void draw_piece(solar_os_gfx_t *gfx, int cx, int cy, int8_t piece)
{
    if (piece == 0) return;
    const bool is_white = piece > 0;
    const int type = abs(piece);

    solar_os_gfx_set_color(gfx, is_white ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_BLACK);

    switch (type) {
    case 1: /* Pawn */
        if (is_white) {
            solar_os_gfx_circle(gfx, cx, cy - 4, 3);
            solar_os_gfx_line(gfx, cx - 4, cy + 6, cx + 4, cy + 6);
            solar_os_gfx_line(gfx, cx - 2, cy - 1, cx - 3, cy + 5);
            solar_os_gfx_line(gfx, cx + 2, cy - 1, cx + 3, cy + 5);
        } else {
            solar_os_gfx_fill_circle(gfx, cx, cy - 4, 3);
            solar_os_gfx_fill_rect(gfx, cx - 3, cy - 1, 6, 6);
            solar_os_gfx_fill_rect(gfx, cx - 5, cy + 5, 10, 2);
        }
        break;

    case 2: /* Knight */
        if (is_white) {
            solar_os_gfx_rect(gfx, cx - 4, cy - 6, 8, 12);
            solar_os_gfx_line(gfx, cx - 4, cy - 2, cx - 6, cy + 1);
            solar_os_gfx_line(gfx, cx - 6, cy + 1, cx - 4, cy + 6);
        } else {
            solar_os_gfx_fill_rect(gfx, cx - 4, cy - 6, 8, 12);
            solar_os_gfx_fill_rect(gfx, cx - 6, cy - 2, 4, 6);
        }
        break;

    case 3: /* Bishop */
        if (is_white) {
            solar_os_gfx_circle(gfx, cx, cy - 3, 5);
            solar_os_gfx_line(gfx, cx, cy - 8, cx, cy + 5);
            solar_os_gfx_line(gfx, cx - 5, cy + 6, cx + 5, cy + 6);
        } else {
            solar_os_gfx_fill_circle(gfx, cx, cy - 3, 5);
            solar_os_gfx_fill_rect(gfx, cx - 5, cy + 5, 10, 3);
        }
        break;

    case 4: /* Rook */
        if (is_white) {
            solar_os_gfx_rect(gfx, cx - 5, cy - 5, 10, 11);
            solar_os_gfx_line(gfx, cx - 5, cy - 5, cx - 5, cy - 8);
            solar_os_gfx_line(gfx, cx, cy - 5, cx, cy - 8);
            solar_os_gfx_line(gfx, cx + 5, cy - 5, cx + 5, cy - 8);
        } else {
            solar_os_gfx_fill_rect(gfx, cx - 5, cy - 5, 10, 11);
            solar_os_gfx_fill_rect(gfx, cx - 6, cy - 8, 3, 4);
            solar_os_gfx_fill_rect(gfx, cx + 3, cy - 8, 3, 4);
        }
        break;

    case 5: /* Queen */
        if (is_white) {
            solar_os_gfx_circle(gfx, cx, cy - 2, 6);
            solar_os_gfx_circle(gfx, cx - 5, cy - 7, 2);
            solar_os_gfx_circle(gfx, cx, cy - 8, 2);
            solar_os_gfx_circle(gfx, cx + 5, cy - 7, 2);
            solar_os_gfx_line(gfx, cx - 6, cy + 6, cx + 6, cy + 6);
        } else {
            solar_os_gfx_fill_circle(gfx, cx, cy - 2, 6);
            solar_os_gfx_fill_circle(gfx, cx - 5, cy - 7, 2);
            solar_os_gfx_fill_circle(gfx, cx, cy - 8, 2);
            solar_os_gfx_fill_circle(gfx, cx + 5, cy - 7, 2);
            solar_os_gfx_fill_rect(gfx, cx - 6, cy + 5, 12, 3);
        }
        break;

    case 6: /* King */
        solar_os_gfx_line(gfx, cx, cy - 8, cx, cy - 3);
        solar_os_gfx_line(gfx, cx - 3, cy - 6, cx + 3, cy - 6);
        if (is_white) {
            solar_os_gfx_circle(gfx, cx, cy + 1, 5);
            solar_os_gfx_line(gfx, cx - 6, cy + 6, cx + 6, cy + 6);
        } else {
            solar_os_gfx_fill_circle(gfx, cx, cy + 1, 5);
            solar_os_gfx_fill_rect(gfx, cx - 6, cy + 5, 12, 3);
        }
        break;
    }
}

static void chess_render(solar_os_context_t *ctx)
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
    solar_os_gfx_text(gfx, 8, 16, "SOLAR CHESS");

    char turn_hdr[32];
    snprintf(turn_hdr, sizeof(turn_hdr), "MOVE %u: %s", (unsigned)chess.move_count, chess.white_turn ? "WHITE" : "BLACK");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t th_w = solar_os_gfx_text_width(gfx, turn_hdr);
    solar_os_gfx_text(gfx, screen_w - (int)th_w - 8, 16, turn_hdr);

    /* 2. 8x8 Board (X: 20..244, Y: 34..258, cell_size: 28px) */
    const int bx = 22;
    const int by = 34;
    const int sq = 28;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, bx - 2, by - 2, 8 * sq + 4, 8 * sq + 4);

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            const int sx = bx + c * sq;
            const int sy = by + r * sq;
            const bool is_dark = ((r + c) % 2 == 1);
            const bool is_cursor = (r == chess.cursor_r && c == chess.cursor_c);
            const bool is_sel = (chess.selected && r == chess.sel_r && c == chess.sel_c);

            if (is_dark) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                for (int py = sy; py < sy + sq; py += 2) {
                    for (int px = sx; px < sx + sq; px += 2) {
                        solar_os_gfx_line(gfx, px, py, px, py);
                    }
                }
            }

            if (is_sel) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, sx, sy, sq, sq);
            }

            draw_piece(gfx, sx + sq / 2, sy + sq / 2, chess.board[r][c]);

            if (is_cursor) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_rect(gfx, sx, sy, sq, sq);
                solar_os_gfx_rect(gfx, sx + 1, sy + 1, sq - 2, sq - 2);
            }
        }
    }

    /* 3. Right Sidebar: Status & Instructions (X: 256..390) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 256, 34, screen_w - 264, 228);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 266, 56, "GAME STATUS");
    solar_os_gfx_line(gfx, 260, 62, screen_w - 14, 62);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 266, 85, chess.status_msg);

    char turn_txt[48];
    snprintf(turn_txt, sizeof(turn_txt), "Turn: %s", chess.white_turn ? "White (Light)" : "Black (Dark)");
    solar_os_gfx_text(gfx, 266, 115, turn_txt);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 266, 150, "CONTROLS:");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 266, 172, "[ARROWS] Move Cursor");
    solar_os_gfx_text(gfx, 266, 192, "[ENTER] Select / Move");
    solar_os_gfx_text(gfx, 266, 212, "[R] Restart Game");
    solar_os_gfx_text(gfx, 266, 232, "[ESC] Exit to Desktop");

    /* 4. Footer */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 293, "[ARROWS] Cursor | [ENTER] Move | [R] New Game | [ESC] Exit");

    solar_os_gfx_present(gfx);
}

static esp_err_t chess_start(solar_os_context_t *ctx)
{
    chess_init_board();
    solar_os_context_set_graphics_active(ctx, true);
    chess_render(ctx);
    return ESP_OK;
}

static void chess_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool chess_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W' || ch == 'k' || ch == 'K') {
            if (chess.cursor_r > 0) chess.cursor_r--;
            chess_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S' || ch == 'j' || ch == 'J') {
            if (chess.cursor_r < 7) chess.cursor_r++;
            chess_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A' || ch == 'h' || ch == 'H') {
            if (chess.cursor_c > 0) chess.cursor_c--;
            chess_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D' || ch == 'l' || ch == 'L') {
            if (chess.cursor_c < 7) chess.cursor_c++;
            chess_render(ctx);
            return true;
        }

        if (ch == '\r' || ch == '\n' || ch == ' ') {
            const int r = chess.cursor_r;
            const int c = chess.cursor_c;

            if (!chess.selected) {
                int8_t p = chess.board[r][c];
                if ((chess.white_turn && p > 0) || (!chess.white_turn && p < 0)) {
                    chess.selected = true;
                    chess.sel_r = r;
                    chess.sel_c = c;
                    strlcpy(chess.status_msg, "Select destination square", sizeof(chess.status_msg));
                }
            } else {
                if (r == chess.sel_r && c == chess.sel_c) {
                    chess.selected = false;
                    strlcpy(chess.status_msg, "Move cancelled", sizeof(chess.status_msg));
                } else {
                    /* Execute Move */
                    chess.board[r][c] = chess.board[chess.sel_r][chess.sel_c];
                    chess.board[chess.sel_r][chess.sel_c] = 0;
                    chess.selected = false;
                    chess.white_turn = !chess.white_turn;
                    if (chess.white_turn) chess.move_count++;
                    snprintf(chess.status_msg, sizeof(chess.status_msg), "%s's turn", chess.white_turn ? "White" : "Black");
                }
            }
            chess_render(ctx);
            return true;
        }

        if (ch == 'r' || ch == 'R') {
            chess_init_board();
            chess_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_chess_app = {
    .name = "chess",
    .summary = "classic 8x8 solar chess game",
    .flags = 0,
    .start = chess_start,
    .stop = chess_stop,
    .event = chess_event,
    .state_slot = &chess_state_ptr,
    .state_size = sizeof(chess_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = CHESS_STACK_SIZE,
};
