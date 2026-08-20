#include "solar_os_chess.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os.h"
#include "solar_os_appbar.h"
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"
#include "solar_os_storage.h"

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
    /* Distinct rising/falling siren so a check is unmistakable vs a move. */
    solar_os_audio_play_tone(1500, 70, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(1000, 70, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(1500, 110, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
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

#define CHESS_VIZ_MAX 16  /* candidate arrows kept on screen during AI think */

typedef struct { int8_t fr, fc, tr, tc; int eval; } chess_viz_move_t;

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
    int cpu_depth; /* AI search depth: 1=Easy, 2=Medium, 3=Hard, 4=Expert */
    /* Chess clock -- per-move wall-time accounting so it works in both 2P
     * and vs-CPU modes: the time a side actually spends (including the AI's
     * blocking think) is charged to it when its move completes. */
    uint32_t white_ms;
    uint32_t black_ms;
    int64_t move_start_us; /* wall time the side to move began its turn */
    bool check_blink;      /* toggles ~every 400ms to flash the king in check */
    /* Move history codes (algebraic, e.g. "e2e4" / "e7xd6"). */
    char last_white[8];
    char last_black[8];
    char best_code[8];     /* the move the CPU just played */
    /* Training mode: while the AI thinks, each candidate move it evaluates
     * is drawn as an arrow (older ones fading to gray) with the running best
     * shown as text, so the player can learn how the engine weighs options. */
    bool training_mode;
    chess_viz_move_t viz_hist[CHESS_VIZ_MAX];
    int viz_count;         /* valid entries in viz_hist; 0 = nothing to draw */
    chess_viz_move_t viz_pv[6]; /* look-ahead line from the current candidate */
    int viz_pv_count;
    char think_best[24];   /* running best move + score during the search */
    int result;            /* 0=ongoing, 1=White win, 2=Black win, 3=draw */
    bool recorded;         /* high score already saved for this game */
    bool show_scores;      /* high-score overlay is open */
    int64_t game_start_us; /* wall time the game began (for duration) */
    bool clock_disabled;   /* clock counting turned off (tap a clock) */
    /* Castling rights (lost once the king or that rook leaves home). */
    bool castle_wk, castle_wq, castle_bk, castle_bq;
    /* Pawn-promotion picker: active while a human pawn awaits a choice. */
    bool promo_active;
    int promo_r, promo_c;
} chess_state_t;

#define CHESS_CLOCK_START_MS 300000U /* 5:00 per side */

static void *chess_state_ptr;
#define chess (*(chess_state_t *)chess_state_ptr)

static const char *chess_difficulty_name(int depth);
static void chess_render(solar_os_context_t *ctx);
static void chess_move_code(int fr, int fc, int tr, int tc, bool capture, char *buf, size_t len);
static void chess_clock_charge(void);
static uint32_t chess_clock_display(bool is_white);
static void chess_promo_box(solar_os_gfx_t *gfx, int *bx, int *by, int *ow, int *oh);

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

static bool chess_can_castle(bool is_white, bool kingside)
{
    const int home = is_white ? 7 : 0;
    const int8_t king = is_white ? 6 : -6;
    const int8_t rook = is_white ? 4 : -4;
    const bool by_white = !is_white; /* attacker colour */
    bool right = is_white ? (kingside ? chess.castle_wk : chess.castle_wq)
                          : (kingside ? chess.castle_bk : chess.castle_bq);
    if (!right) return false;
    if (chess.board[home][4] != king) return false;
    if (is_king_in_check(chess.board, is_white)) return false; /* can't castle out of check */
    if (kingside) {
        if (chess.board[home][7] != rook) return false;
        if (chess.board[home][5] != 0 || chess.board[home][6] != 0) return false;
        if (is_square_attacked(chess.board, home, 5, by_white)) return false;
        if (is_square_attacked(chess.board, home, 6, by_white)) return false;
    } else {
        if (chess.board[home][0] != rook) return false;
        if (chess.board[home][1] != 0 || chess.board[home][2] != 0 || chess.board[home][3] != 0) return false;
        if (is_square_attacked(chess.board, home, 3, by_white)) return false;
        if (is_square_attacked(chess.board, home, 2, by_white)) return false;
    }
    return true;
}

/* Drop castling rights when a king or rook leaves (or is captured on) its home square. */
static void chess_touch_castle(int r, int c)
{
    if (r == 7 && c == 4) { chess.castle_wk = chess.castle_wq = false; }
    else if (r == 7 && c == 0) chess.castle_wq = false;
    else if (r == 7 && c == 7) chess.castle_wk = false;
    else if (r == 0 && c == 4) { chess.castle_bk = chess.castle_bq = false; }
    else if (r == 0 && c == 0) chess.castle_bq = false;
    else if (r == 0 && c == 7) chess.castle_bk = false;
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

    /* Castling: offer the two-square king move when rights and geometry allow. */
    const int8_t sel = chess.board[chess.sel_r][chess.sel_c];
    if (abs(sel) == 6) {
        const bool is_white = sel > 0;
        const int home = is_white ? 7 : 0;
        if (chess.sel_r == home && chess.sel_c == 4) {
            if (chess_can_castle(is_white, true))  chess.legal_targets[home][6] = true;
            if (chess_can_castle(is_white, false)) chess.legal_targets[home][2] = true;
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
    /* Yield periodically so a deep (Expert) search can't starve the idle
     * task and trip the watchdog -- this was the source of occasional
     * lock-ups. Does not affect the search result. */
    static uint32_t node_counter = 0;
    if ((++node_counter & 0x3fffu) == 0) {
        vTaskDelay(1);
    }
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

/* Greedy 1-ply best move for a side, scored by evaluate_board -- used only
 * to sketch a plausible look-ahead line for the training overlay, not for
 * the actual engine decision. */
static bool chess_greedy_best(bool is_white, int *ofr, int *ofc, int *otr, int *otc)
{
    int best = is_white ? -999999 : 999999;
    bool found = false;
    for (int fr = 0; fr < 8; fr++)
        for (int fc = 0; fc < 8; fc++) {
            const int8_t p = chess.board[fr][fc];
            if (p == 0 || (is_white ? p < 0 : p > 0)) continue;
            for (int tr = 0; tr < 8; tr++)
                for (int tc = 0; tc < 8; tc++) {
                    if (!is_legal_move(chess.board, fr, fc, tr, tc, is_white)) continue;
                    int8_t saved = chess.board[tr][tc];
                    int8_t moving = chess.board[fr][fc];
                    if (moving == 1 && tr == 0) moving = 5;
                    if (moving == -1 && tr == 7) moving = -5;
                    chess.board[tr][tc] = moving;
                    chess.board[fr][fc] = 0;
                    int sc = evaluate_board(chess.board);
                    chess.board[fr][fc] = (abs(moving) == 5 && (tr == 0 || tr == 7)) ? (is_white ? 1 : -1) : moving;
                    chess.board[tr][tc] = saved;
                    if (is_white ? (sc > best) : (sc < best)) {
                        best = sc; *ofr = fr; *ofc = fc; *otr = tr; *otc = tc; found = true;
                    }
                }
        }
    return found;
}

/* Training look-ahead: from the given candidate (Black) move, walk a greedy
 * principal line `depth` plies deep, drawing each step. Board is snapshotted. */
static void chess_viz_lookahead(solar_os_context_t *ctx, int fr, int fc, int tr, int tc, int depth)
{
    int8_t snap[8][8];
    memcpy(snap, chess.board, sizeof(snap));
    int8_t mv = chess.board[fr][fc];
    if (mv == -1 && tr == 7) mv = -5;
    chess.board[tr][tc] = mv;
    chess.board[fr][fc] = 0;
    bool white = true;
    chess.viz_pv_count = 0;
    for (int k = 0; k < depth && k < (int)(sizeof(chess.viz_pv) / sizeof(chess.viz_pv[0])); k++) {
        int a, bb, cc, dd;
        if (!chess_greedy_best(white, &a, &bb, &cc, &dd)) break;
        int8_t m = chess.board[a][bb];
        if (m == 1 && cc == 0) m = 5;
        if (m == -1 && cc == 7) m = -5;
        chess.board[cc][dd] = m;
        chess.board[a][bb] = 0;
        chess.viz_pv[k] = (chess_viz_move_t){ (int8_t)a, (int8_t)bb, (int8_t)cc, (int8_t)dd, 0 };
        chess.viz_pv_count = k + 1;
        chess_render(ctx);
        vTaskDelay(pdMS_TO_TICKS(70));
        white = !white;
    }
    memcpy(chess.board, snap, sizeof(snap));
    chess.viz_pv_count = 0;
}

static bool perform_cpu_move(solar_os_context_t *ctx)
{
    int best_val = 99999;
    int bfr = -1, bfc = -1, btr = -1, btc = -1;
    const bool viz = chess.training_mode && ctx != NULL;

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

                            int ev = minimax(chess.board, chess.cpu_depth > 0 ? chess.cpu_depth : 2, -99999, 99999, true);

                            chess.board[fr][fc] = (moving == -5 && tr == 7) ? -1 : moving;
                            chess.board[tr][tc] = saved;

                            const bool improved = (ev < best_val);
                            if (improved) {
                                best_val = ev;
                                bfr = fr; bfc = fc; btr = tr; btc = tc;
                            }

                            /* Training mode: accumulate the considered moves
                             * as a fading fan of arrows (older = lighter) and
                             * show the running best as text. Lower score is
                             * better for Black, the CPU. */
                            if (viz) {
                                const chess_viz_move_t entry = {
                                    (int8_t)fr, (int8_t)fc, (int8_t)tr, (int8_t)tc, ev };
                                if (chess.viz_count < CHESS_VIZ_MAX) {
                                    chess.viz_hist[chess.viz_count++] = entry;
                                } else {
                                    memmove(&chess.viz_hist[0], &chess.viz_hist[1],
                                            (CHESS_VIZ_MAX - 1) * sizeof(chess.viz_hist[0]));
                                    chess.viz_hist[CHESS_VIZ_MAX - 1] = entry;
                                }
                                snprintf(chess.think_best, sizeof(chess.think_best),
                                         "%c%c%c%c=%d best %d",
                                         (char)('a' + fc), (char)('8' - fr),
                                         (char)('a' + tc), (char)('8' - tr),
                                         ev, best_val);
                                chess_render(ctx);
                                vTaskDelay(pdMS_TO_TICKS(improved ? 220 : 70));
                                if (improved) {
                                    /* Only sketch the deep line for a new best,
                                     * to keep the visualization time bounded. */
                                    chess_viz_lookahead(ctx, fr, fc, tr, tc,
                                                        chess.cpu_depth > 0 ? chess.cpu_depth : 2);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (viz) {
        chess.viz_count = 0;
        chess.think_best[0] = ' ';
    }

    if (bfr != -1) {
        int8_t moving = chess.board[bfr][bfc];
        int8_t captured = chess.board[btr][btc];
        const bool was_capture = (captured != 0);
        if (captured > 0 && captured <= 5) {
            chess.captured_b[captured]++;
        }
        /* Record CPU move code (Black's last move and the chosen best). */
        chess_move_code(bfr, bfc, btr, btc, was_capture, chess.best_code, sizeof(chess.best_code));
        strlcpy(chess.last_black, chess.best_code, sizeof(chess.last_black));
        if (moving == -1 && btr == 7) moving = -5; /* Promotion to Queen */
        chess_touch_castle(bfr, bfc);              /* CPU king/rook move */
        chess_touch_castle(btr, btc);              /* or capture of a rook */
        chess.board[btr][btc] = moving;
        chess.board[bfr][bfc] = 0;
        chess.cursor_r = btr;
        chess.cursor_c = btc;
        chess_clock_charge();      /* charge Black the full think time */
        chess.white_turn = true;
        chess.move_count++;

        chess.in_check = is_king_in_check(chess.board, true);
        if (chess.in_check) {
            if (!has_any_legal_moves(chess.board, true)) {
                chess.game_over = true;
                chess.result = 2;
                strlcpy(chess.status_msg, "Checkmate! Black Wins!", sizeof(chess.status_msg));
            } else {
                strlcpy(chess.status_msg, "CHECK! White King in Danger!", sizeof(chess.status_msg));
            }
        } else if (!has_any_legal_moves(chess.board, true)) {
            chess.game_over = true;
            chess.result = 3;
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
    const int saved_depth = chess.cpu_depth;
    memset(&chess, 0, sizeof(chess));
    chess.cpu_depth = saved_depth > 0 ? saved_depth : 2; /* keep chosen difficulty */
    chess.white_ms = CHESS_CLOCK_START_MS;
    chess.black_ms = CHESS_CLOCK_START_MS;
    chess.move_start_us = 0;
    chess.check_blink = false;
    chess.viz_count = 0;
    chess.cursor_r = 6;
    chess.cursor_c = 4;
    chess.sel_r = -1;
    chess.sel_c = -1;
    chess.white_turn = true;
    chess.vs_cpu = true;
    chess.move_count = 1;
    strlcpy(chess.status_msg, "White's turn to move", sizeof(chess.status_msg));

    /* memset cleared these; a new game starts with full castling rights. */
    chess.castle_wk = chess.castle_wq = true;
    chess.castle_bk = chess.castle_bq = true;
    chess.promo_active = false;
    chess.game_start_us = esp_timer_get_time();

    static const int8_t init_row[8] = {4, 2, 3, 5, 6, 3, 2, 4};
    for (int c = 0; c < 8; c++) {
        chess.board[0][c] = -init_row[c]; /* Black back row */
        chess.board[1][c] = -1;           /* Black pawns */
        chess.board[6][c] = 1;            /* White pawns */
        chess.board[7][c] = init_row[c];  /* White back row */
    }
}

/* Strokes a closed polygon outline in the current color (fill_polygon has no
 * outline variant). */
static void chess_stroke_poly(solar_os_gfx_t *gfx, const solar_os_gfx_point_t *pts, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const solar_os_gfx_point_t *a = &pts[i];
        const solar_os_gfx_point_t *b = &pts[(i + 1) % n];
        solar_os_gfx_line(gfx, a->x, a->y, b->x, b->y);
    }
}

/* Renders one silhouette polygon so a white and a black piece of the same
 * type are the SAME shape, differing only in fill: black pieces are solid
 * black; white pieces are white-filled with a black outline so they read on
 * both light and dark squares. */
static void chess_draw_silhouette(solar_os_gfx_t *gfx, const solar_os_gfx_point_t *pts,
                                  size_t n, bool is_white)
{
    if (is_white) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_polygon(gfx, pts, n);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        chess_stroke_poly(gfx, pts, n);
    } else {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_polygon(gfx, pts, n);
    }
}

static void draw_piece(solar_os_gfx_t *gfx, int cx, int cy, int8_t piece)
{
    if (piece == 0) return;
    const bool is_white = piece > 0;
    const int type = abs(piece);
    /* Detail color: contrasts against the piece body so cut-outs (bishop
     * slit, rook crenels, king cross) stay visible on either colour. */
    const solar_os_gfx_color_t detail = is_white ? SOLAR_OS_GFX_COLOR_BLACK
                                                  : SOLAR_OS_GFX_COLOR_WHITE;

    switch (type) {
    case 1: { /* Pawn: round head over a flared body on a base. */
        const solar_os_gfx_point_t body[] = {
            {cx - 2, cy - 3}, {cx + 2, cy - 3}, {cx + 4, cy + 3},
            {cx + 6, cy + 7}, {cx - 6, cy + 7}, {cx - 4, cy + 3},
        };
        chess_draw_silhouette(gfx, body, 6, is_white);
        if (is_white) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_circle(gfx, cx, cy - 5, 3);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_circle(gfx, cx, cy - 5, 3);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_circle(gfx, cx, cy - 5, 3);
        }
        break;
    }

    case 2: { /* Knight: a left-facing horse head on a base. */
        const solar_os_gfx_point_t horse[] = {
            {cx - 7, cy + 1},   /* muzzle tip */
            {cx - 6, cy + 4},   /* under jaw */
            {cx - 5, cy + 7},   /* base left */
            {cx + 6, cy + 7},   /* base right */
            {cx + 5, cy + 1},   /* back of neck */
            {cx + 4, cy - 4},   /* mane */
            {cx + 3, cy - 9},   /* ear tip */
            {cx + 1, cy - 5},   /* ear notch */
            {cx - 1, cy - 7},   /* poll / forehead top */
            {cx - 4, cy - 3},   /* brow */
            {cx - 6, cy - 1},   /* nose bridge */
        };
        chess_draw_silhouette(gfx, horse, 11, is_white);
        /* Eye */
        solar_os_gfx_set_color(gfx, detail);
        solar_os_gfx_fill_rect(gfx, cx - 2, cy - 3, 2, 2);
        break;
    }

    case 3: { /* Bishop: pointed mitre over a round body, with a slit. */
        const solar_os_gfx_point_t mitre[] = {
            {cx, cy - 9}, {cx + 3, cy - 4}, {cx + 4, cy + 1},
            {cx + 6, cy + 7}, {cx - 6, cy + 7}, {cx - 4, cy + 1},
            {cx - 3, cy - 4},
        };
        chess_draw_silhouette(gfx, mitre, 7, is_white);
        /* Top ball + the diagonal slit */
        if (is_white) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_circle(gfx, cx, cy - 9, 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_circle(gfx, cx, cy - 9, 2);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_circle(gfx, cx, cy - 9, 2);
        }
        solar_os_gfx_set_color(gfx, detail);
        solar_os_gfx_line(gfx, cx - 2, cy - 2, cx + 2, cy - 6);
        break;
    }

    case 4: { /* Rook: crenellated castle tower. */
        const solar_os_gfx_point_t rook[] = {
            {cx - 6, cy + 7}, {cx - 6, cy - 3}, {cx - 6, cy - 7},
            {cx - 4, cy - 7}, {cx - 4, cy - 5}, {cx - 2, cy - 5},
            {cx - 2, cy - 7}, {cx + 2, cy - 7}, {cx + 2, cy - 5},
            {cx + 4, cy - 5}, {cx + 4, cy - 7}, {cx + 6, cy - 7},
            {cx + 6, cy - 3}, {cx + 6, cy + 7},
        };
        chess_draw_silhouette(gfx, rook, 14, is_white);
        break;
    }

    case 5: { /* Queen: crown of five points topped by balls, flared body. */
        const solar_os_gfx_point_t body[] = {
            {cx - 5, cy - 3}, {cx + 5, cy - 3}, {cx + 6, cy + 7}, {cx - 6, cy + 7},
        };
        chess_draw_silhouette(gfx, body, 4, is_white);
        const solar_os_gfx_point_t crown[] = {
            {cx - 6, cy - 3}, {cx - 6, cy - 8}, {cx - 3, cy - 4},
            {cx, cy - 9}, {cx + 3, cy - 4}, {cx + 6, cy - 8}, {cx + 6, cy - 3},
        };
        chess_draw_silhouette(gfx, crown, 7, is_white);
        /* Balls on the crown points */
        const int ball_x[3] = {cx - 6, cx, cx + 6};
        const int ball_y[3] = {cy - 8, cy - 9, cy - 8};
        for (int i = 0; i < 3; i++) {
            if (is_white) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
                solar_os_gfx_fill_circle(gfx, ball_x[i], ball_y[i], 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_circle(gfx, ball_x[i], ball_y[i], 2);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_circle(gfx, ball_x[i], ball_y[i], 2);
            }
        }
        break;
    }

    case 6: { /* King: cross on top over a flared body. */
        const solar_os_gfx_point_t body[] = {
            {cx - 4, cy - 3}, {cx + 4, cy - 3}, {cx + 6, cy + 7}, {cx - 6, cy + 7},
        };
        chess_draw_silhouette(gfx, body, 4, is_white);
        /* Head */
        if (is_white) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_circle(gfx, cx, cy - 2, 4);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_circle(gfx, cx, cy - 2, 4);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_circle(gfx, cx, cy - 2, 4);
        }
        /* Cross */
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_line(gfx, cx, cy - 11, cx, cy - 5);
        solar_os_gfx_line(gfx, cx - 2, cy - 9, cx + 2, cy - 9);
        break;
    }
    }
}

/* Draws one side's captured pieces as compact glyphs, left to right by
 * type, collapsing multiples into a single glyph with an "xN" count to save
 * space, and wrapping to a second row when the first fills up. `piece_sign`
 * is +1 to render the captured pieces as white glyphs, -1 for black.
 * `counts[t]` is how many of piece-type t (1=P..5=Q) were captured.
 * Entries are drawn across rows at y_row0 then y_row1. */
static void chess_draw_captured_row(solar_os_gfx_t *gfx, int x, int y_row0, int y_row1,
                                    const int counts[6], int piece_sign, int max_x)
{
    int cx = x + 9;
    int cy = y_row0;
    bool on_first_row = true;

    for (int type = 1; type <= 5; type++) {
        if (counts[type] <= 0) continue;

        int entry_w = 15;
        char cnt[8] = {0};
        if (counts[type] > 1) {
            snprintf(cnt, sizeof(cnt), "x%d", counts[type]);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            entry_w += (int)solar_os_gfx_text_width(gfx, cnt) + 6;
        }

        /* Wrap to the second row once this entry would overflow. */
        if (cx + entry_w > max_x) {
            if (!on_first_row) break; /* both rows full */
            on_first_row = false;
            cx = x + 9;
            cy = y_row1;
            if (cx + entry_w > max_x) break;
        }

        draw_piece(gfx, cx, cy, (int8_t)(piece_sign * type));
        cx += 15;
        if (cnt[0] != '\0') {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, cx - 2, cy + 4, cnt);
            cx += (int)solar_os_gfx_text_width(gfx, cnt) + 6;
        }
    }
}

/* Formats a move as a compact algebraic code: from-square, 'x' if it is a
 * capture else '-', to-square. e.g. "e2-e4" or "e7xd6". */
static void chess_move_code(int fr, int fc, int tr, int tc, bool capture, char *buf, size_t len)
{
    snprintf(buf, len, "%c%c%c%c%c",
             (char)('a' + fc), (char)('8' - fr),
             capture ? 'x' : '-',
             (char)('a' + tc), (char)('8' - tr));
}

/* Charges the side that just moved (chess.white_turn, called BEFORE the turn
 * flips) the wall time it spent, and restarts the clock for the next side.
 * A side that runs out loses on time. */
static void chess_clock_charge(void)
{
    const int64_t now = esp_timer_get_time();
    if (chess.clock_disabled) { chess.move_start_us = now; return; }
    if (chess.move_start_us == 0) { chess.move_start_us = now; return; }
    const uint32_t elapsed = (uint32_t)((now - chess.move_start_us) / 1000);
    uint32_t *clk = chess.white_turn ? &chess.white_ms : &chess.black_ms;
    if (*clk <= elapsed) {
        *clk = 0;
        chess.game_over = true;
        chess.result = chess.white_turn ? 2 : 1;
        snprintf(chess.status_msg, sizeof(chess.status_msg), "Time out! %s wins.",
                 chess.white_turn ? "Black" : "White");
    } else {
        *clk -= elapsed;
    }
    chess.move_start_us = now;
}

/* Remaining time to show for one side: the committed value, minus the live
 * elapsed time while it is that side's turn (so a human's clock ticks down
 * visibly between moves). */
static uint32_t chess_clock_display(bool is_white)
{
    uint32_t base = is_white ? chess.white_ms : chess.black_ms;
    if (!chess.clock_disabled && !chess.game_over && chess.move_start_us != 0 && chess.white_turn == is_white) {
        const uint32_t el = (uint32_t)((esp_timer_get_time() - chess.move_start_us) / 1000);
        return el >= base ? 0 : base - el;
    }
    return base;
}

/* Finds the king of the given colour; returns false if not on the board. */
static bool chess_find_king(bool is_white, int *out_r, int *out_c)
{
    const int8_t king = is_white ? 6 : -6;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (chess.board[r][c] == king) { *out_r = r; *out_c = c; return true; }
        }
    }
    return false;
}

static void chess_format_clock(uint32_t ms, char *buf, size_t len)
{
    const unsigned total = ms / 1000U;
    snprintf(buf, len, "%u:%02u", total / 60U, total % 60U);
}

/* ------------------------------------------------------------------
 * High scores: top-5 White-vs-CPU wins, persisted on flash (works
 * without an SD card via solar_os_storage_default_path).
 * ------------------------------------------------------------------ */
#define CHESS_SCORE_COUNT 5
#define CHESS_SCORE_MAGIC 0x43485331u /* 'CHS1' */

typedef struct {
    uint32_t score;
    uint32_t duration_s;
    int32_t  level;
    int64_t  when_epoch;
} chess_score_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    chess_score_t entries[CHESS_SCORE_COUNT];
} chess_score_file_t;

static void chess_scores_load(chess_score_file_t *out)
{
    memset(out, 0, sizeof(*out));
    out->magic = CHESS_SCORE_MAGIC;
    out->version = 1;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_default_path(".chess/scores.dat", path, sizeof(path)) != ESP_OK) return;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return;
    chess_score_file_t tmp;
    const size_t n = fread(&tmp, 1, sizeof(tmp), f);
    fclose(f);
    if (n == sizeof(tmp) && tmp.magic == CHESS_SCORE_MAGIC) *out = tmp;
}

static void chess_scores_save(const chess_score_file_t *sf)
{
    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_default_path(".chess", dir, sizeof(dir)) == ESP_OK) {
        (void)solar_os_storage_mkdir(dir);
    }
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_default_path(".chess/scores.dat", path, sizeof(path)) != ESP_OK) return;
    FILE *f = fopen(path, "wb");
    if (f == NULL) return;
    (void)fwrite(sf, 1, sizeof(*sf), f);
    fclose(f);
}

/* Inserts a new score keeping the table sorted high-to-low, capped at 5. */
static void chess_scores_insert(chess_score_file_t *sf, const chess_score_t *e)
{
    int pos = CHESS_SCORE_COUNT;
    for (int i = 0; i < CHESS_SCORE_COUNT; i++) {
        if (e->score > sf->entries[i].score) { pos = i; break; }
    }
    if (pos >= CHESS_SCORE_COUNT) return;
    for (int i = CHESS_SCORE_COUNT - 1; i > pos; i--) sf->entries[i] = sf->entries[i - 1];
    sf->entries[pos] = *e;
}

/* Records a finished game if the human (White) beat the CPU. Score rewards a
 * higher AI level and more clock left at the finish. */
static void chess_record_score(void)
{
    if (chess.result != 1 || !chess.vs_cpu) return;
    const uint32_t remaining_s = chess.white_ms / 1000U;
    const uint32_t duration_s =
        (uint32_t)((esp_timer_get_time() - chess.game_start_us) / 1000000);
    chess_score_t e = {
        .score = (uint32_t)chess.cpu_depth * 1000U + remaining_s * 10U,
        .duration_s = duration_s,
        .level = chess.cpu_depth,
        .when_epoch = (int64_t)time(NULL),
    };
    chess_score_file_t sf;
    chess_scores_load(&sf);
    chess_scores_insert(&sf, &e);
    chess_scores_save(&sf);
}

static void chess_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header */
    char turn_hdr[48];
    snprintf(turn_hdr, sizeof(turn_hdr), "Move %u: %s [%s]",
             (unsigned)chess.move_count,
             chess.white_turn ? "White" : "Black",
             chess.vs_cpu ? "vs CPU" : "2-Player");
    solar_os_appbar_header_t header = {0};
    header.title = "Chess";
    header.show_back = true;
    header.status_line = turn_hdr;
    solar_os_appbar_draw_header(gfx, &header);

    /* 2. 8x8 Board (X: 18..242, cell_size: 28px) */
    const int bx = 20;
    const int by = solar_os_appbar_header_height(gfx) + solar_os_appbar_status_line_height(gfx) + 6;
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

    int king_r = -1, king_c = -1;
    if (chess.in_check) {
        chess_find_king(chess.white_turn, &king_r, &king_c);
    }

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            const int sx = bx + c * sq;
            const int sy = by + r * sq;
            const bool is_dark = ((r + c) % 2 == 1);
            const bool is_check_king = (r == king_r && c == king_c);
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

            /* Flash the checked king's square */
            if (is_check_king && chess.check_blink) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
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

            /* Bold triple border marking the king in check */
            if (is_check_king) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_rect(gfx, sx, sy, sq, sq);
                solar_os_gfx_rect(gfx, sx + 1, sy + 1, sq - 2, sq - 2);
                solar_os_gfx_rect(gfx, sx + 2, sy + 2, sq - 4, sq - 4);
            }
        }
    }

    /* 3. Right Sidebar (X: 256..screen_w) */
    const int footer_h = solar_os_appbar_footer_height(gfx);
    const int sidebar_h = screen_h - footer_h - by - 4;
    const int panel_x = 258;
    const int panel_w = screen_w - panel_x - 6;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 256, by, screen_w - 264, sidebar_h);

    /* --- Two clocks side by side: left = White (black text on white),
     * right = Black (white text on black). No letters -- the contrasting
     * colors say which is which. Active side gets an inner frame. --- */
    char wclk[12], bclk[12];
    chess_format_clock(chess_clock_display(true), wclk, sizeof(wclk));
    chess_format_clock(chess_clock_display(false), bclk, sizeof(bclk));
    if (chess.clock_disabled) { strlcpy(wclk, "off", sizeof(wclk)); strlcpy(bclk, "off", sizeof(bclk)); }
    const int clk_h = 26;
    const int clk_y = by + 4;
    const int half = panel_w / 2;

    /* Left: White clock */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, panel_x, clk_y, half, clk_h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, panel_x, clk_y, half, clk_h);
    if (chess.white_turn && !chess.game_over) solar_os_gfx_rect(gfx, panel_x + 1, clk_y + 1, half - 2, clk_h - 2);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_text(gfx, panel_x + (half - (int)solar_os_gfx_text_width(gfx, wclk)) / 2, clk_y + 18, wclk);

    /* Right: Black clock */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, panel_x + half, clk_y, panel_w - half, clk_h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    if (!chess.white_turn && !chess.game_over) solar_os_gfx_rect(gfx, panel_x + half + 1, clk_y + 1, panel_w - half - 2, clk_h - 2);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_text(gfx, panel_x + half + (panel_w - half - (int)solar_os_gfx_text_width(gfx, bclk)) / 2, clk_y + 18, bclk);

    /* --- One compact mode|turn line (no "Mode"/"Turn" labels) --- */
    int iy = clk_y + clk_h + 15;
    char info[48];
    const char *turn_side = chess.white_turn ? "White" : "Black";
    const char *cpu_tag = (chess.vs_cpu && !chess.white_turn) ? "(CPU)" : "";
    if (chess.vs_cpu) {
        const char lvl = chess.cpu_depth <= 1 ? '1' : chess.cpu_depth == 2 ? '2'
                       : chess.cpu_depth == 3 ? '3' : 'E';
        snprintf(info, sizeof(info), "AI:%c | %s%s", lvl, turn_side, cpu_tag);
    } else {
        snprintf(info, sizeof(info), "2P | %s", turn_side);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, panel_x + 2, iy, info);

    /* --- Move codes: best (CPU choice), then White / Black last moves --- */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    iy += 18;
    char line[40];
    snprintf(line, sizeof(line), "Best %s", chess.best_code[0] ? chess.best_code : "-");
    solar_os_gfx_text(gfx, panel_x + 2, iy, line);
    iy += 15;
    snprintf(line, sizeof(line), "W %s  B %s",
             chess.last_white[0] ? chess.last_white : "-",
             chess.last_black[0] ? chess.last_black : "-");
    solar_os_gfx_text(gfx, panel_x + 2, iy, line);

    /* --- AI thinking best (only while searching) --- */
    iy += 15;
    if (chess.think_best[0] != ' ') {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        char tline[40];
        snprintf(tline, sizeof(tline), "AI: %s", chess.think_best);
        solar_os_gfx_text(gfx, panel_x + 2, iy, tline);
    }

    /* Captured-pieces tray fills the rest of the sidebar. */
    const int cap_top = iy + 6;
    const int cap_bottom = by + sidebar_h - 6;
    const int cap_split = (cap_top + cap_bottom) / 2;
    const int cap_max_x = screen_w - 16;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 260, cap_top, screen_w - 14, cap_top);
    solar_os_gfx_line(gfx, 260, cap_split, screen_w - 14, cap_split);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 266, cap_top + 12, "White took:");
    chess_draw_captured_row(gfx, 266, cap_top + 26, cap_top + 42, chess.captured_w, -1, cap_max_x);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 266, cap_split + 12, "Black took:");
    chess_draw_captured_row(gfx, 266, cap_split + 26, cap_split + 42, chess.captured_b, 1, cap_max_x);

    /* 4. Footer */
    solar_os_appbar_shortcut_t footer_items[4];
    footer_items[0] = (solar_os_appbar_shortcut_t){ .key = 'm', .ctrl = false, .label = "AI Mode" };
    footer_items[1] = (solar_os_appbar_shortcut_t){ .key = 'g', .ctrl = false, .label = "Level" };
    footer_items[2] = (solar_os_appbar_shortcut_t){ .key = 'e', .ctrl = false, .label = "Learn" };
    if (chess.training_mode) strlcpy(footer_items[2].label, "Learn*", sizeof(footer_items[2].label));
    footer_items[3] = (solar_os_appbar_shortcut_t){ .key = 'r', .ctrl = false, .label = "New" };
    const solar_os_appbar_shortcuts_t footer_shortcuts = { .items = footer_items, .count = 4 };
    solar_os_appbar_draw_footer(gfx, &footer_shortcuts);

    /* Training-mode overlay: the fan of candidate moves the AI has weighed,
     * each shaded by its evaluation (better-for-Black = darker), the running
     * best drawn solid black + thick, plus the greedy look-ahead line. */
    if (chess.viz_count > 0) {
        /* Range of evals seen, to map score -> shade. Black wants the
         * lowest eval, so lowest = darkest. */
        int best_ev = chess.viz_hist[0].eval, worst_ev = chess.viz_hist[0].eval;
        for (int i = 1; i < chess.viz_count; i++) {
            if (chess.viz_hist[i].eval < best_ev) best_ev = chess.viz_hist[i].eval;
            if (chess.viz_hist[i].eval > worst_ev) worst_ev = chess.viz_hist[i].eval;
        }
        const int span = (worst_ev > best_ev) ? (worst_ev - best_ev) : 1;
        for (int i = 0; i < chess.viz_count; i++) {
            const chess_viz_move_t *m = &chess.viz_hist[i];
            const int fx = bx + m->fc * sq + sq / 2;
            const int fy = by + m->fr * sq + sq / 2;
            const int tx = bx + m->tc * sq + sq / 2;
            const int ty = by + m->tr * sq + sq / 2;
            const bool is_best = (m->eval == best_ev);
            if (is_best) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_line(gfx, fx, fy, tx, ty);
                solar_os_gfx_line(gfx, fx + 1, fy, tx + 1, ty);
                solar_os_gfx_line(gfx, fx, fy + 1, tx, ty + 1);
                solar_os_gfx_fill_circle(gfx, tx, ty, 4);
            } else {
                /* Worse move -> lighter. Map eval within [best,worst]. */
                int lvl = 3 + (11 * (m->eval - best_ev)) / span;
                if (lvl < 3) lvl = 3; if (lvl > 15) lvl = 15;
                solar_os_gfx_set_color(gfx, solar_os_gfx_gray((uint8_t)lvl));
                solar_os_gfx_line(gfx, fx, fy, tx, ty);
            }
        }
        /* Greedy look-ahead line from the current best candidate, drawn as a
         * light dashed-looking chain so it reads as 'reading ahead'. */
        for (int k = 0; k < chess.viz_pv_count; k++) {
            const chess_viz_move_t *m = &chess.viz_pv[k];
            const int fx = bx + m->fc * sq + sq / 2;
            const int fy = by + m->fr * sq + sq / 2;
            const int tx = bx + m->tc * sq + sq / 2;
            const int ty = by + m->tr * sq + sq / 2;
            solar_os_gfx_set_color(gfx, solar_os_gfx_gray(8));
            solar_os_gfx_line(gfx, fx, fy, tx, ty);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_circle(gfx, tx, ty, 3);
        }
    }

    /* Winner banner: big blinking text over the board when the game ends. */
    if (chess.game_over && chess.result != 0) {
        const char *msg = chess.result == 1 ? "WHITE WINS"
                        : chess.result == 2 ? "BLACK WINS" : "DRAW";
        const int bw = 8 * sq;
        const int band_y = by + 4 * sq - 22;
        if (chess.check_blink) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, bx, band_y, bw, 44);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_rect(gfx, bx, band_y, bw, 44);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, bx, band_y, bw, 44);
            solar_os_gfx_rect(gfx, bx + 1, band_y + 1, bw - 2, 42);
        }
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
        const int tw = (int)solar_os_gfx_text_width(gfx, msg);
        solar_os_gfx_text(gfx, bx + (bw - tw) / 2, band_y + 29, msg);
    }

    /* High-score overlay (top 5), opened by tapping the bottom bar. */
    if (chess.show_scores) {
        chess_score_file_t sf;
        chess_scores_load(&sf);
        const int ox = 24, oy = by + 8, ow = screen_w - 48, oh = 8 * sq - 20;
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, ox, oy, ow, oh);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, ox, oy, ow, oh);
        solar_os_gfx_rect(gfx, ox + 1, oy + 1, ow - 2, oh - 2);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
        solar_os_gfx_text(gfx, ox + 10, oy + 22, "TOP 5 SCORES (You vs CPU)");
        solar_os_gfx_line(gfx, ox + 6, oy + 28, ox + ow - 6, oy + 28);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
        for (int i = 0; i < CHESS_SCORE_COUNT; i++) {
            const chess_score_t *e = &sf.entries[i];
            const int ry = oy + 48 + i * 22;
            char row[64];
            if (e->score == 0) {
                snprintf(row, sizeof(row), "%d. ---", i + 1);
            } else {
                char dt[24] = "--";
                time_t t = (time_t)e->when_epoch;
                struct tm *tmv = localtime(&t);
                if (tmv != NULL) strftime(dt, sizeof(dt), "%y-%m-%d %H:%M", tmv);
                const char lvl = e->level <= 1 ? '1' : e->level == 2 ? '2'
                               : e->level == 3 ? '3' : 'E';
                snprintf(row, sizeof(row), "%d. %-14s %u:%02u Lv%c %lu",
                         i + 1, dt, e->duration_s / 60U, e->duration_s % 60U,
                         lvl, (unsigned long)e->score);
            }
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_text(gfx, ox + 10, ry, row);
        }
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, ox + 10, oy + oh - 8, "Tap anywhere to close");
    }

    /* Promotion picker overlay: four choices centred on the board. */
    if (chess.promo_active) {
        int pbx, pby, pow, poh;
        chess_promo_box(gfx, &pbx, &pby, &pow, &poh);
        const char *labels[4] = { "Q", "R", "B", "N" };
        /* Backing panel + title band above the row. */
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, pbx - 4, pby - 24, pow * 4 + 8, poh + 28);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, pbx - 4, pby - 24, pow * 4 + 8, poh + 28);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
        solar_os_gfx_text(gfx, pbx, pby - 8, "Promote to:");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
        for (int i = 0; i < 4; i++) {
            const int ox = pbx + i * pow;
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_rect(gfx, ox + 2, pby + 2, pow - 4, poh - 4);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, ox + 2, pby + 2, pow - 4, poh - 4);
            const int lw = (int)solar_os_gfx_text_width(gfx, labels[i]);
            solar_os_gfx_text(gfx, ox + (pow - lw) / 2, pby + poh / 2 + 8, labels[i]);
        }
    }

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

/* Cycles the CPU search depth Easy(1) -> Medium(2) -> Hard(3) -> Easy.
 * Deeper search plays stronger but takes longer to "think" per move. */
static const char *chess_difficulty_name(int depth)
{
    switch (depth) {
    case 1:  return "Easy";
    case 2:  return "Medium";
    case 3:  return "Hard";
    default: return "Expert";
    }
}

static void chess_cycle_difficulty(void)
{
    chess.cpu_depth = (chess.cpu_depth >= 4) ? 1 : chess.cpu_depth + 1;
    snprintf(chess.status_msg, sizeof(chess.status_msg), "Difficulty: %s (deeper = slower)",
             chess_difficulty_name(chess.cpu_depth));
}

/* Selects a piece, moves it, or cancels a selection at (r, c) -- the same
 * action Enter performs on the cursor square. Shared by the keyboard
 * handler and the click handler's board-tap path so they can never
 * disagree about what a square tap does. */
/* Complete a move: charge the clock, flip sides, test for check/mate, and
 * trigger the CPU reply. Shared by normal moves, castling, and promotion. */
static void chess_finish_move(solar_os_context_t *ctx)
{
    chess_clock_charge();
    chess.white_turn = !chess.white_turn;
    if (chess.white_turn) chess.move_count++;

    chess.in_check = is_king_in_check(chess.board, chess.white_turn);
    if (chess.in_check) {
        if (!has_any_legal_moves(chess.board, chess.white_turn)) {
            chess.game_over = true;
            chess.result = chess.white_turn ? 2 : 1;
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
        chess.result = 3;
        strlcpy(chess.status_msg, "Stalemate! Draw Game", sizeof(chess.status_msg));
    } else {
        snprintf(chess.status_msg, sizeof(chess.status_msg), "%s's turn",
                 chess.white_turn ? "White" : "Black");
    }

    chess_render(ctx);

    if (chess.vs_cpu && !chess.white_turn && !chess.game_over) {
        strlcpy(chess.status_msg, "CPU thinking...", sizeof(chess.status_msg));
        chess_render(ctx);
        perform_cpu_move(ctx);
        chess_render(ctx);
    }
}

/* Promotion picker geometry: four side-by-side option boxes (Q R B N),
 * centred on the board. Returns the option under (px,py), or -1. */
static const int8_t chess_promo_types[4] = { 5, 4, 3, 2 }; /* Q R B N */
static void chess_promo_box(solar_os_gfx_t *gfx, int *bx, int *by, int *ow, int *oh)
{
    (void)gfx;
    const int board_x = 20, sq = 28;
    const int board_y = solar_os_appbar_header_height(gfx) + solar_os_appbar_status_line_height(gfx) + 6;
    *ow = 44; *oh = 56;
    *bx = board_x + 4 * sq - (*ow * 4) / 2;
    *by = board_y + 4 * sq - *oh / 2;
}
static int chess_promo_hit(solar_os_gfx_t *gfx, int px, int py)
{
    int bx, by, ow, oh;
    chess_promo_box(gfx, &bx, &by, &ow, &oh);
    if (py < by || py >= by + oh) return -1;
    if (px < bx || px >= bx + ow * 4) return -1;
    return (px - bx) / ow;
}
static void chess_apply_promotion(solar_os_context_t *ctx, int option)
{
    if (option < 0 || option > 3) return;
    const int8_t sign = (chess.promo_r == 0) ? 1 : -1;
    chess.board[chess.promo_r][chess.promo_c] = (int8_t)(sign * chess_promo_types[option]);
    chess.promo_active = false;
    chess_sfx_move();
    chess_finish_move(ctx);
}

static void chess_activate_square(solar_os_context_t *ctx, int r, int c)
{
    if (chess.game_over) {
        chess_init_board();
        chess_render(ctx);
        return;
    }

    chess.cursor_r = r;
    chess.cursor_c = c;

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

            /* Record the move code before mutating the board. */
            char code[8];
            chess_move_code(chess.sel_r, chess.sel_c, r, c, was_capture, code, sizeof(code));
            if (chess.white_turn) strlcpy(chess.last_white, code, sizeof(chess.last_white));
            else strlcpy(chess.last_black, code, sizeof(chess.last_black));

            /* King/rook departure or capture drops castling rights. */
            chess_touch_castle(chess.sel_r, chess.sel_c);
            chess_touch_castle(r, c);

            /* Castling: the king slides two squares, the rook jumps beside it. */
            if (abs(moving) == 6 && abs(c - chess.sel_c) == 2) {
                const int home = r;
                chess.board[r][c] = moving;
                chess.board[chess.sel_r][chess.sel_c] = 0;
                if (c == 6) {          /* king-side */
                    chess.board[home][5] = chess.board[home][7];
                    chess.board[home][7] = 0;
                } else {               /* queen-side */
                    chess.board[home][3] = chess.board[home][0];
                    chess.board[home][0] = 0;
                }
                chess.selected = false;
                memset(chess.legal_targets, 0, sizeof(chess.legal_targets));
                chess_sfx_move();
                chess_finish_move(ctx);
                return;
            }

            /* Move the piece. A promotion pauses for the picker. */
            const bool is_promo = (moving == 1 && r == 0) || (moving == -1 && r == 7);
            chess.board[r][c] = moving;
            chess.board[chess.sel_r][chess.sel_c] = 0;
            chess.selected = false;
            memset(chess.legal_targets, 0, sizeof(chess.legal_targets));

            if (was_capture) {
                chess_sfx_capture();
            } else {
                chess_sfx_move();
            }

            if (is_promo) {
                chess.promo_active = true;
                chess.promo_r = r;
                chess.promo_c = c;
                strlcpy(chess.status_msg, "Promote: tap Q R B N", sizeof(chess.status_msg));
                chess_render(ctx);
                return;
            }

            chess_finish_move(ctx);
            return;
        } else {
            strlcpy(chess.status_msg, "Illegal move! Follow rules", sizeof(chess.status_msg));
            chess_sfx_illegal();
        }
    }
    chess_render(ctx);
}

static bool chess_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (chess.game_over) return true;
        const int64_t now_us = esp_timer_get_time();
        bool need_render = false;

        /* Flash the king-in-check ~2.5 Hz. */
        const bool phase = ((now_us / 400000) & 1) != 0;
        if (chess.in_check && phase != chess.check_blink) {
            chess.check_blink = phase;
            need_render = true;
        }

        /* Live clock: recompute the side-to-move's displayed time so it
         * ticks down between moves. (The AI's think time is charged in
         * chess_clock_charge() at move completion, since it blocks ticks.) */
        if (chess.move_start_us == 0) {
            chess.move_start_us = now_us;
        } else {
            static uint32_t last_shown_sec = 0xffffffffU;
            const uint32_t rem = chess_clock_display(chess.white_turn);
            if (rem == 0) {
                chess_clock_charge();
                chess.game_over = true;
                chess.result = chess.white_turn ? 2 : 1;
                snprintf(chess.status_msg, sizeof(chess.status_msg),
                         "Time out! %s wins.", chess.white_turn ? "Black" : "White");
                chess_sfx_checkmate();
                need_render = true;
            } else if (rem / 1000U != last_shown_sec) {
                last_shown_sec = rem / 1000U;
                need_render = true;
            }
        }

        if (need_render) chess_render(ctx);
        return true;
    }


    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;

        if (chess.promo_active) {
            int opt = chess_promo_hit(gfx, event->data.click.x, event->data.click.y);
            if (opt >= 0) chess_apply_promotion(ctx, opt);
            return true;
        }

        if (chess.show_scores) {
            chess.show_scores = false;
            chess_render(ctx);
            return true;
        }

        solar_os_appbar_header_t header = {0};
        header.show_back = true;
        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, event->data.click.x, event->data.click.y, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                solar_os_context_request_exit(ctx);
            }
            return true;
        }

        /* Tap either clock box to toggle clock counting on/off. */
        {
            const int cby = solar_os_appbar_header_height(gfx) + solar_os_appbar_status_line_height(gfx) + 6;
            const int cpanel_x = 258;
            const int cpanel_w = (int)solar_os_gfx_width(gfx) - cpanel_x - 6;
            if (event->data.click.x >= cpanel_x && event->data.click.x < cpanel_x + cpanel_w &&
                event->data.click.y >= cby + 4 && event->data.click.y < cby + 4 + 26) {
                chess.clock_disabled = !chess.clock_disabled;
                chess.move_start_us = esp_timer_get_time();
                chess_render(ctx);
                return true;
            }
        }

        /* Mirrors chess_render()'s 8x8 board geometry. */
        const int bx = 20;
        const int by = solar_os_appbar_header_height(gfx) + solar_os_appbar_status_line_height(gfx) + 6;
        const int sq = 28;
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;
        if (px >= bx && px < bx + 8 * sq && py >= by && py < by + 8 * sq) {
            const int c = (px - bx) / sq;
            const int r = (py - by) / sq;
            chess_activate_square(ctx, r, c);
            return true;
        }

        solar_os_appbar_shortcut_t footer_items[4];
        footer_items[0] = (solar_os_appbar_shortcut_t){ .key = 'm', .ctrl = false, .label = "AI Mode" };
        footer_items[1] = (solar_os_appbar_shortcut_t){ .key = 'g', .ctrl = false, .label = "Level" };
        footer_items[2] = (solar_os_appbar_shortcut_t){ .key = 'e', .ctrl = false, .label = "Learn" };
        if (chess.training_mode) strlcpy(footer_items[2].label, "Learn*", sizeof(footer_items[2].label));
        footer_items[3] = (solar_os_appbar_shortcut_t){ .key = 'r', .ctrl = false, .label = "New" };
        const solar_os_appbar_shortcuts_t footer_shortcuts = { .items = footer_items, .count = 4 };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &footer_shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM) {
                if (fhit.index == 0) {
                    chess.vs_cpu = !chess.vs_cpu;
                    snprintf(chess.status_msg, sizeof(chess.status_msg), "Mode: %s",
                             chess.vs_cpu ? "1P vs CPU" : "2P Local");
                } else if (fhit.index == 1) {
                    chess_cycle_difficulty();
                } else if (fhit.index == 2) {
                    chess.training_mode = !chess.training_mode;
                    snprintf(chess.status_msg, sizeof(chess.status_msg), "Training mode: %s",
                             chess.training_mode ? "ON (shows AI moves)" : "OFF");
                } else {
                    chess_init_board();
                }
            } else {
                /* Tapped the footer bar but not a chip -> show high scores. */
                chess.show_scores = true;
            }
            chess_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const uint8_t u_ch = (uint8_t)event->data.ch;
        const char ch = event->data.ch;

        /* Promotion picker: pick with Q/R/B/N (Enter/Space = Queen). */
        if (chess.promo_active) {
            if (ch == 'q' || ch == 'Q' || ch == '\r' || ch == '\n' || ch == ' ') chess_apply_promotion(ctx, 0);
            else if (ch == 'r' || ch == 'R') chess_apply_promotion(ctx, 1);
            else if (ch == 'b' || ch == 'B') chess_apply_promotion(ctx, 2);
            else if (ch == 'n' || ch == 'N') chess_apply_promotion(ctx, 3);
            return true;
        }

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
            chess_activate_square(ctx, chess.cursor_r, chess.cursor_c);
            return true;
        }

        if (ch == 'e' || ch == 'E') {
            chess.training_mode = !chess.training_mode;
            snprintf(chess.status_msg, sizeof(chess.status_msg), "Training mode: %s",
                     chess.training_mode ? "ON (shows AI moves)" : "OFF");
            chess_render(ctx);
            return true;
        }

        if (ch == 'g' || ch == 'G') {
            chess_cycle_difficulty();
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
    .tick_interval_ms = 200U,
    .worker_stack_bytes = CHESS_STACK_SIZE,
};

