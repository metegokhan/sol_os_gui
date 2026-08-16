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
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"

#define CHESS_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(CHESS_STACK_SIZE);

static void chess_sfx_move(void)
{
    solar_os_audio_play_tone(850, 18, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void chess_sfx_capture(void)
{
    solar_os_audio_play_tone(400, 30, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(250, 45, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void chess_sfx_check(void)
{
    solar_os_audio_play_tone(1200, 35, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(1500, 60, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void chess_sfx_checkmate(void)
{
    solar_os_audio_play_tone(523, 60, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(659, 60, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(784, 80, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(1046, 140, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void chess_sfx_illegal(void)
{
    solar_os_audio_play_tone(200, 60, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

/* Pieces: 0=Empty, 1=P, 2=N, 3=B, 4=R, 5=Q, 6=K. Positive=White, Negative=Black */
typedef struct {
    int8_t board[8][8];
    int cursor_r;
    int cursor_c;
    int sel_r;
    int sel_c;
    bool selected;
    bool white_turn;
    bool vs_cpu;
    bool game_over;
    bool in_check;
    bool legal_targets[8][8];
    char status_msg[64];
    uint32_t move_count;
    int captured_w[6]; /* P, N, B, R, Q */
    int captured_b[6];
} chess_state_t;

static void *chess_state_ptr;
#define chess (*(chess_state_t *)chess_state_ptr)

/* Piece-Square Tables for AI positional play */
static const int8_t pawn_table[8][8] = {
    { 0,  0,  0,  0,  0,  0,  0,  0},
    {50, 50, 50, 50, 50, 50, 50, 50},
    {10, 10, 20, 30, 30, 20, 10, 10},
    { 5,  5, 10, 25, 25, 10,  5,  5},
    { 0,  0,  0, 20, 20,  0,  0,  0},
    { 5, -5,-10,  0,  0,-10, -5,  5},
    { 5, 10, 10,-20,-20, 10, 10,  5},
    { 0,  0,  0,  0,  0,  0,  0,  0}
};

static const int8_t knight_table[8][8] = {
    {-50,-40,-30,-30,-30,-30,-40,-50},
    {-40,-20,  0,  0,  0,  0,-20,-40},
    {-30,  0, 10, 15, 15, 10,  0,-30},
    {-30,  5, 15, 20, 20, 15,  5,-30},
    {-30,  0, 15, 20, 20, 15,  0,-30},
    {-30,  5, 10, 15, 15, 10,  5,-30},
    {-40,-20,  0,  5,  5,  0,-20,-40},
    {-50,-40,-30,-30,-30,-30,-40,-50}
};

static const int piece_vals[7] = {0, 100, 320, 330, 500, 900, 20000};

static bool is_valid_pseudo_move(const int8_t b[8][8], int fr, int fc, int tr, int tc, bool is_white)
{
    if (fr < 0 || fr > 7 || fc < 0 || fc > 7 || tr < 0 || tr > 7 || tc < 0 || tc > 7) return false;
    if (fr == tr && fc == tc) return false;

    const int8_t piece = b[fr][fc];
    if (piece == 0) return false;
    if (is_white && piece < 0) return false;
    if (!is_white && piece > 0) return false;

    const int8_t dest = b[tr][tc];
    if (is_white && dest > 0) return false;
    if (!is_white && dest < 0) return false;

    const int ptype = abs(piece);
    const int dr = tr - fr;
    const int dc = tc - fc;
    const int abs_dr = abs(dr);
    const int abs_dc = abs(dc);

    switch (ptype) {
    case 1: { /* Pawn */
        const int forward = is_white ? -1 : 1;
        const int start_r = is_white ? 6 : 1;

        if (dc == 0) {
            /* 1 square forward */
            if (dr == forward && dest == 0) return true;
            /* 2 squares forward from start rank */
            if (fr == start_r && dr == 2 * forward && dest == 0 && b[fr + forward][fc] == 0) return true;
        } else if (abs_dc == 1 && dr == forward) {
            /* Diagonal capture */
            if (is_white && dest < 0) return true;
            if (!is_white && dest > 0) return true;
        }
        return false;
    }

    case 2: /* Knight */
        return (abs_dr == 1 && abs_dc == 2) || (abs_dr == 2 && abs_dc == 1);

    case 3: /* Bishop */
        if (abs_dr != abs_dc) return false;
        {
            const int step_r = dr > 0 ? 1 : -1;
            const int step_c = dc > 0 ? 1 : -1;
            int r = fr + step_r;
            int c = fc + step_c;
            while (r != tr || c != tc) {
                if (b[r][c] != 0) return false;
                r += step_r;
                c += step_c;
            }
        }
        return true;

    case 4: /* Rook */
        if (dr != 0 && dc != 0) return false;
        {
            const int step_r = dr == 0 ? 0 : (dr > 0 ? 1 : -1);
            const int step_c = dc == 0 ? 0 : (dc > 0 ? 1 : -1);
            int r = fr + step_r;
            int c = fc + step_c;
            while (r != tr || c != tc) {
                if (b[r][c] != 0) return false;
                r += step_r;
                c += step_c;
            }
        }
        return true;

    case 5: /* Queen */
        if (abs_dr == abs_dc) {
            const int step_r = dr > 0 ? 1 : -1;
            const int step_c = dc > 0 ? 1 : -1;
            int r = fr + step_r;
            int c = fc + step_c;
            while (r != tr || c != tc) {
                if (b[r][c] != 0) return false;
                r += step_r;
                c += step_c;
            }
            return true;
        } else if (dr == 0 || dc == 0) {
            const int step_r = dr == 0 ? 0 : (dr > 0 ? 1 : -1);
            const int step_c = dc == 0 ? 0 : (dc > 0 ? 1 : -1);
            int r = fr + step_r;
            int c = fc + step_c;
            while (r != tr || c != tc) {
                if (b[r][c] != 0) return false;
                r += step_r;
                c += step_c;
            }
            return true;
        }
        return false;

    case 6: /* King */
        return abs_dr <= 1 && abs_dc <= 1;

    default:
        return false;
    }
}

static bool is_square_attacked(const int8_t b[8][8], int r, int c, bool by_white)
{
    for (int fr = 0; fr < 8; fr++) {
        for (int fc = 0; fc < 8; fc++) {
            const int8_t p = b[fr][fc];
            if (p == 0) continue;
            if (by_white && p > 0) {
                if (p == 1) {
                    /* White Pawn attacks diagonally up (fr - 1) */
                    if (fr - 1 == r && (fc - 1 == c || fc + 1 == c)) return true;
                } else if (p == 6) {
                    if (abs(fr - r) <= 1 && abs(fc - c) <= 1) return true;
                } else {
                    if (is_valid_pseudo_move(b, fr, fc, r, c, true)) return true;
                }
            } else if (!by_white && p < 0) {
                if (p == -1) {
                    /* Black Pawn attacks diagonally down (fr + 1) */
                    if (fr + 1 == r && (fc - 1 == c || fc + 1 == c)) return true;
                } else if (p == -6) {
                    if (abs(fr - r) <= 1 && abs(fc - c) <= 1) return true;
                } else {
                    if (is_valid_pseudo_move(b, fr, fc, r, c, false)) return true;
                }
            }
        }
    }
    return false;
}

static bool is_king_in_check(const int8_t b[8][8], bool is_white)
{
    const int8_t king_val = is_white ? 6 : -6;
    int kr = -1, kc = -1;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (b[r][c] == king_val) {
                kr = r;
                kc = c;
                break;
            }
        }
        if (kr != -1) break;
    }
    if (kr == -1) return false;
    return is_square_attacked(b, kr, kc, !is_white);
}

static bool is_legal_move(const int8_t b[8][8], int fr, int fc, int tr, int tc, bool is_white)
{
    if (!is_valid_pseudo_move(b, fr, fc, tr, tc, is_white)) return false;

    /* Simulate move */
    int8_t temp[8][8];
    memcpy(temp, b, sizeof(temp));
    temp[tr][tc] = temp[fr][fc];
    temp[fr][fc] = 0;

    return !is_king_in_check(temp, is_white);
}

static void update_legal_targets(void)
{
    memset(chess.legal_targets, 0, sizeof(chess.legal_targets));
    if (!chess.selected || chess.sel_r < 0 || chess.sel_c < 0) return;

    for (int tr = 0; tr < 8; tr++) {
        for (int tc = 0; tc < 8; tc++) {
            if (is_legal_move(chess.board, chess.sel_r, chess.sel_c, tr, tc, chess.white_turn)) {
                chess.legal_targets[tr][tc] = true;
            }
        }
    }
}

static bool has_any_legal_moves(const int8_t b[8][8], bool is_white)
{
    for (int fr = 0; fr < 8; fr++) {
        for (int fc = 0; fc < 8; fc++) {
            const int8_t p = b[fr][fc];
            if ((is_white && p > 0) || (!is_white && p < 0)) {
                for (int tr = 0; tr < 8; tr++) {
                    for (int tc = 0; tc < 8; tc++) {
                        if (is_legal_move(b, fr, fc, tr, tc, is_white)) return true;
                    }
                }
            }
        }
    }
    return false;
}

/* ------------------- AI Engine (Minimax with Alpha-Beta) ------------------- */

static int evaluate_board(const int8_t b[8][8])
{
    int score = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            const int8_t p = b[r][c];
            if (p == 0) continue;
            const int ptype = abs(p);
            int val = piece_vals[ptype];

            if (ptype == 1) {
                val += p > 0 ? pawn_table[r][c] : pawn_table[7 - r][c];
            } else if (ptype == 2) {
                val += p > 0 ? knight_table[r][c] : knight_table[7 - r][c];
            }

            score += (p > 0) ? val : -val;
        }
    }
    return score;
}

static int minimax(int8_t b[8][8], int depth, int alpha, int beta, bool is_maximizing)
{
    if (depth == 0) {
        return evaluate_board(b);
    }

    if (is_maximizing) {
        int max_eval = -99999;
        for (int fr = 0; fr < 8; fr++) {
            for (int fc = 0; fc < 8; fc++) {
                if (b[fr][fc] > 0) {
                    for (int tr = 0; tr < 8; tr++) {
                        for (int tc = 0; tc < 8; tc++) {
                            if (is_legal_move(b, fr, fc, tr, tc, true)) {
                                int8_t saved = b[tr][tc];
                                int8_t moving = b[fr][fc];
                                if (moving == 1 && tr == 0) moving = 5; /* Promotion */
                                b[tr][tc] = moving;
                                b[fr][fc] = 0;

                                int ev = minimax(b, depth - 1, alpha, beta, false);

                                b[fr][fc] = (moving == 5 && tr == 0) ? 1 : moving;
                                b[tr][tc] = saved;

                                if (ev > max_eval) max_eval = ev;
                                if (ev > alpha) alpha = ev;
                                if (beta <= alpha) return max_eval;
                            }
                        }
                    }
                }
            }
        }
        return max_eval == -99999 ? -50000 : max_eval;
    } else {
        int min_eval = 99999;
        for (int fr = 0; fr < 8; fr++) {
            for (int fc = 0; fc < 8; fc++) {
                if (b[fr][fc] < 0) {
                    for (int tr = 0; tr < 8; tr++) {
                        for (int tc = 0; tc < 8; tc++) {
                            if (is_legal_move(b, fr, fc, tr, tc, false)) {
                                int8_t saved = b[tr][tc];
                                int8_t moving = b[fr][fc];
                                if (moving == -1 && tr == 7) moving = -5; /* Promotion */
                                b[tr][tc] = moving;
                                b[fr][fc] = 0;

                                int ev = minimax(b, depth - 1, alpha, beta, true);

                                b[fr][fc] = (moving == -5 && tr == 7) ? -1 : moving;
                                b[tr][tc] = saved;

                                if (ev < min_eval) min_eval = ev;
                                if (ev < beta) beta = ev;
                                if (beta <= alpha) return min_eval;
                            }
                        }
                    }
                }
            }
        }
        return min_eval == 99999 ? 50000 : min_eval;
    }
}

static bool perform_cpu_move(void)
{
    int best_val = 99999;
    int bfr = -1, bfc = -1, btr = -1, btc = -1;

    for (int fr = 0; fr < 8; fr++) {
        for (int fc = 0; fc < 8; fc++) {
            if (chess.board[fr][fc] < 0) {
                for (int tr = 0; tr < 8; tr++) {
                    for (int tc = 0; tc < 8; tc++) {
                        if (is_legal_move(chess.board, fr, fc, tr, tc, false)) {
                            int8_t saved = chess.board[tr][tc];
                            int8_t moving = chess.board[fr][fc];
                            if (moving == -1 && tr == 7) moving = -5;
                            chess.board[tr][tc] = moving;
                            chess.board[fr][fc] = 0;

                            int ev = minimax(chess.board, 2, -99999, 99999, true);

                            chess.board[fr][fc] = (moving == -5 && tr == 7) ? -1 : moving;
                            chess.board[tr][tc] = saved;

                            if (ev < best_val) {
                                best_val = ev;
                                bfr = fr; bfc = fc; btr = tr; btc = tc;
                            }
                        }
                    }
                }
            }
        }
    }

    if (bfr != -1) {
        int8_t moving = chess.board[bfr][bfc];
        int8_t captured = chess.board[btr][btc];
        if (captured > 0 && captured <= 5) {
            chess.captured_b[captured]++;
        }
        if (moving == -1 && btr == 7) moving = -5; /* Promotion to Queen */
        chess.board[btr][btc] = moving;
        chess.board[bfr][bfc] = 0;
        chess.cursor_r = btr;
        chess.cursor_c = btc;
        chess.white_turn = true;
        chess.move_count++;

        chess.in_check = is_king_in_check(chess.board, true);
        if (chess.in_check) {
            if (!has_any_legal_moves(chess.board, true)) {
                chess.game_over = true;
                strlcpy(chess.status_msg, "Checkmate! Black Wins!", sizeof(chess.status_msg));
            } else {
                strlcpy(chess.status_msg, "CHECK! White King in Danger!", sizeof(chess.status_msg));
            }
        } else if (!has_any_legal_moves(chess.board, true)) {
            chess.game_over = true;
            strlcpy(chess.status_msg, "Stalemate! Draw Game!", sizeof(chess.status_msg));
        } else {
            strlcpy(chess.status_msg, "Your turn (White)", sizeof(chess.status_msg));
        }
        return true;
    }
    return false;
}

static void chess_init_board(void)
{
    memset(&chess, 0, sizeof(chess));
    chess.cursor_r = 6;
    chess.cursor_c = 4;
    chess.sel_r = -1;
    chess.sel_c = -1;
    chess.white_turn = true;
    chess.vs_cpu = true;
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

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

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
            solar_os_gfx_circle(gfx, cx, cy - 8, 2);
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
    solar_os_gfx_text(gfx, 8, 16, "SOLAR CHESS PRO");

    char turn_hdr[48];
    snprintf(turn_hdr, sizeof(turn_hdr), "MOVE %u: %s [%s]",
             (unsigned)chess.move_count,
             chess.white_turn ? "WHITE" : "BLACK",
             chess.vs_cpu ? "VS CPU" : "2-PLAYER");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t th_w = solar_os_gfx_text_width(gfx, turn_hdr);
    solar_os_gfx_text(gfx, screen_w - (int)th_w - 8, 16, turn_hdr);

    /* 2. 8x8 Board (X: 18..242, Y: 30..254, cell_size: 28px) */
    const int bx = 20;
    const int by = 32;
    const int sq = 28;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, bx - 2, by - 2, 8 * sq + 4, 8 * sq + 4);

    /* Rank & File coordinate markings */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    for (int i = 0; i < 8; i++) {
        char rank_ch[2] = {(char)('8' - i), '\0'};
        char file_ch[2] = {(char)('A' + i), '\0'};
        solar_os_gfx_text(gfx, bx - 12, by + i * sq + 18, rank_ch);
        solar_os_gfx_text(gfx, bx + i * sq + 10, by + 8 * sq + 12, file_ch);
    }

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            const int sx = bx + c * sq;
            const int sy = by + r * sq;
            const bool is_dark = ((r + c) % 2 == 1);
            const bool is_cursor = (r == chess.cursor_r && c == chess.cursor_c);
            const bool is_sel = (chess.selected && r == chess.sel_r && c == chess.sel_c);
            const bool is_target = chess.legal_targets[r][c];

            /* Chessboard checker pattern */
            if (is_dark) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                for (int py = sy; py < sy + sq; py += 2) {
                    for (int px = sx; px < sx + sq; px += 2) {
                        solar_os_gfx_line(gfx, px, py, px, py);
                    }
                }
            }

            /* Highlight selected origin square */
            if (is_sel) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, sx, sy, sq, sq);
            }

            /* Draw piece */
            draw_piece(gfx, sx + sq / 2, sy + sq / 2, chess.board[r][c]);

            /* Draw legal destination target hint */
            if (is_target) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                if (chess.board[r][c] != 0) {
                    /* Capture target: square corners */
                    solar_os_gfx_rect(gfx, sx + 2, sy + 2, sq - 4, sq - 4);
                } else {
                    /* Move target: center dot */
                    solar_os_gfx_fill_circle(gfx, sx + sq / 2, sy + sq / 2, 3);
                }
            }

            /* Highlight cursor */
            if (is_cursor) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_rect(gfx, sx, sy, sq, sq);
                solar_os_gfx_rect(gfx, sx + 1, sy + 1, sq - 2, sq - 2);
            }
        }
    }

    /* 3. Right Sidebar: Status & Instructions (X: 256..390) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 256, 32, screen_w - 264, 236);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 266, 52, "GAME STATUS");
    solar_os_gfx_line(gfx, 260, 58, screen_w - 14, 58);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 266, 78, chess.status_msg);

    char mode_txt[48];
    snprintf(mode_txt, sizeof(mode_txt), "Mode: %s", chess.vs_cpu ? "1P vs CPU (AI)" : "2P Pass & Play");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 266, 102, mode_txt);

    char turn_txt[48];
    snprintf(turn_txt, sizeof(turn_txt), "Turn: %s", chess.white_turn ? "White (Player)" : (chess.vs_cpu ? "Black (CPU)" : "Black"));
    solar_os_gfx_text(gfx, 266, 122, turn_txt);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 266, 150, "CONTROLS:");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 266, 170, "[ARROWS] Move Cursor");
    solar_os_gfx_text(gfx, 266, 188, "[ENTER] Select / Move");
    solar_os_gfx_text(gfx, 266, 206, "[M] Toggle 1P/2P Mode");
    solar_os_gfx_text(gfx, 266, 224, "[R] Restart Game");
    solar_os_gfx_text(gfx, 266, 242, "[ESC] Exit Game");

    /* 4. Footer */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 293, "[ARROWS] Cursor | [ENTER] Move | [M] AI Mode | [R] New | [ESC] Exit");

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
        const uint8_t u_ch = (uint8_t)event->data.ch;
        const char ch = event->data.ch;

        /* 1. If piece is selected: ESC, Backspace, Delete, C, or X cancels selection */
        if (chess.selected) {
            if (u_ch == SOLAR_OS_KEY_ESCAPE || u_ch == 8 || u_ch == 127 || u_ch == SOLAR_OS_KEY_DELETE ||
                ch == 'c' || ch == 'C' || ch == 'x' || ch == 'X') {
                chess.selected = false;
                memset(chess.legal_targets, 0, sizeof(chess.legal_targets));
                strlcpy(chess.status_msg, "Selection cancelled", sizeof(chess.status_msg));
                chess_sfx_illegal();
                chess_render(ctx);
                return true;
            }
        }

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
            if (chess.game_over) {
                chess_init_board();
                chess_render(ctx);
                return true;
            }

            const int r = chess.cursor_r;
            const int c = chess.cursor_c;

            if (!chess.selected) {
                int8_t p = chess.board[r][c];
                if ((chess.white_turn && p > 0) || (!chess.white_turn && p < 0)) {
                    chess.selected = true;
                    chess.sel_r = r;
                    chess.sel_c = c;
                    update_legal_targets();
                    strlcpy(chess.status_msg, "Select destination square [ESC: Cancel]", sizeof(chess.status_msg));
                    chess_sfx_move();
                }
            } else {
                if (r == chess.sel_r && c == chess.sel_c) {
                    chess.selected = false;
                    memset(chess.legal_targets, 0, sizeof(chess.legal_targets));
                    strlcpy(chess.status_msg, "Selection cancelled", sizeof(chess.status_msg));
                    chess_sfx_illegal();
                } else if (chess.legal_targets[r][c]) {
                    /* Execute Legal Move */
                    int8_t moving = chess.board[chess.sel_r][chess.sel_c];
                    int8_t captured = chess.board[r][c];
                    bool was_capture = (captured != 0);

                    if (captured < 0 && captured >= -5) {
                        chess.captured_w[abs(captured)]++;
                    } else if (captured > 0 && captured <= 5) {
                        chess.captured_b[captured]++;
                    }

                    /* Pawn promotion to Queen */
                    if (moving == 1 && r == 0) moving = 5;
                    if (moving == -1 && r == 7) moving = -5;

                    chess.board[r][c] = moving;
                    chess.board[chess.sel_r][chess.sel_c] = 0;
                    chess.selected = false;
                    memset(chess.legal_targets, 0, sizeof(chess.legal_targets));

                    if (was_capture) {
                        chess_sfx_capture();
                    } else {
                        chess_sfx_move();
                    }

                    chess.white_turn = !chess.white_turn;
                    if (chess.white_turn) chess.move_count++;

                    /* Check / Checkmate verification */
                    chess.in_check = is_king_in_check(chess.board, chess.white_turn);
                    if (chess.in_check) {
                        if (!has_any_legal_moves(chess.board, chess.white_turn)) {
                            chess.game_over = true;
                            snprintf(chess.status_msg, sizeof(chess.status_msg), "Checkmate! %s Wins!",
                                     chess.white_turn ? "Black" : "White");
                            chess_sfx_checkmate();
                        } else {
                            snprintf(chess.status_msg, sizeof(chess.status_msg), "CHECK! %s King in danger",
                                     chess.white_turn ? "White" : "Black");
                            chess_sfx_check();
                        }
                    } else if (!has_any_legal_moves(chess.board, chess.white_turn)) {
                        chess.game_over = true;
                        strlcpy(chess.status_msg, "Stalemate! Draw Game", sizeof(chess.status_msg));
                    } else {
                        snprintf(chess.status_msg, sizeof(chess.status_msg), "%s's turn",
                                 chess.white_turn ? "White" : "Black");
                    }

                    chess_render(ctx);

                    /* If VS CPU mode and it's Black's turn, compute CPU response */
                    if (chess.vs_cpu && !chess.white_turn && !chess.game_over) {
                        strlcpy(chess.status_msg, "CPU thinking...", sizeof(chess.status_msg));
                        chess_render(ctx);
                        perform_cpu_move();
                        chess_render(ctx);
                    }
                    return true;
                } else {
                    strlcpy(chess.status_msg, "Illegal move! Follow rules", sizeof(chess.status_msg));
                    chess_sfx_illegal();
                }
            }
            chess_render(ctx);
            return true;
        }

        if (ch == 'm' || ch == 'M') {
            chess.vs_cpu = !chess.vs_cpu;
            snprintf(chess.status_msg, sizeof(chess.status_msg), "Mode: %s",
                     chess.vs_cpu ? "1P vs CPU" : "2P Local");
            chess_render(ctx);
            return true;
        }

        if (ch == 'r' || ch == 'R') {
            chess_init_board();
            chess_render(ctx);
            return true;
        }

        if (u_ch == SOLAR_OS_KEY_APP_EXIT || u_ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_chess_app = {
    .name = "chess",
    .summary = "classic 8x8 chess with CPU AI engine",
    .flags = 0,
    .start = chess_start,
    .stop = chess_stop,
    .event = chess_event,
    .state_slot = &chess_state_ptr,
    .state_size = sizeof(chess_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = CHESS_STACK_SIZE,
};

