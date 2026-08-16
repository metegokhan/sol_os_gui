#include "solar_os_backgammon.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"

#define BACKGAMMON_STACK_SIZE 10240
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(BACKGAMMON_STACK_SIZE);

/* Sound Effects Helper */
static void bg_sfx_tone(uint32_t freq, uint32_t dur_ms)
{
    solar_os_audio_play_tone(freq, dur_ms, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void bg_sfx_dice_clatter(void)
{
    bg_sfx_tone(700 + (esp_random() % 500), 12);
}

static void bg_sfx_move_tap(void)
{
    bg_sfx_tone(750, 18);
}

static void bg_sfx_hit(void)
{
    bg_sfx_tone(320, 45);
}

static void bg_sfx_bear_off(void)
{
    bg_sfx_tone(1150, 25);
}

static void bg_sfx_win_fanfare(void)
{
    bg_sfx_tone(523, 60);
    bg_sfx_tone(659, 60);
    bg_sfx_tone(784, 80);
    bg_sfx_tone(1046, 140);
}

static void bg_sfx_pass(void)
{
    bg_sfx_tone(280, 80);
}

/* Points 1 to 24.
 * Positive count = White checkers (moving 24 -> 1)
 * Negative count = Black checkers (moving 1 -> 24)
 */
typedef struct {
    int8_t points[25];    /* 1..24 */
    int8_t bar_white;     /* White checkers on bar */
    int8_t bar_black;     /* Black checkers on bar */
    int8_t off_white;     /* White checkers borne off */
    int8_t off_black;     /* Black checkers borne off */

    /* Turn & Dice */
    bool white_turn;
    bool vs_cpu;
    bool game_over;
    bool winner_white;
    bool is_mars;
    bool dice_rolled;

    int dice[4];          /* Up to 4 moves if doubles */
    bool dice_used[4];
    int dice_count;

    /* Dice Roll Animation */
    int roll_anim_frames;
    int anim_dice_disp[2];

    /* Blinking Highlight State */
    bool blink_phase;
    uint32_t last_blink_tick;

    /* Interactive state */
    int cursor_point;     /* 0 = Bar, 1..24 = Points, 25 = Off Tray */
    int selected_from;    /* -1 if nothing selected, 0 for Bar, 1..24 for Point */
    bool legal_targets[26];

    char status_msg[64];
    uint32_t wins_white;
    uint32_t wins_black;

    int64_t cpu_move_at_us;
} backgammon_state_t;

static void *backgammon_state_ptr;
#define bg (*(backgammon_state_t *)backgammon_state_ptr)

static bool backgammon_is_legal_move(int from, int die_val, bool is_white, int *out_to);
static bool backgammon_has_any_legal_moves(bool is_white);
static bool backgammon_point_has_legal_move(int p, bool is_white);
static void backgammon_nav_movable_sources(int dir);
static void backgammon_nav_legal_targets(int dir);
static void backgammon_select_piece(int p);

static void backgammon_init_board(void)
{
    memset(bg.points, 0, sizeof(bg.points));
    bg.bar_white = 0;
    bg.bar_black = 0;
    bg.off_white = 0;
    bg.off_black = 0;

    /* Standard Backgammon Starting Position */
    /* White checkers (moving down towards 1) */
    bg.points[24] = 2;
    bg.points[13] = 5;
    bg.points[8]  = 3;
    bg.points[6]  = 5;

    /* Black checkers (moving up towards 24) */
    bg.points[1]  = -2;
    bg.points[12] = -5;
    bg.points[17] = -3;
    bg.points[19] = -5;

    bg.white_turn = true;
    bg.game_over = false;
    bg.dice_rolled = false;
    bg.roll_anim_frames = 0;
    bg.selected_from = -1;
    bg.cursor_point = 24;
    bg.blink_phase = false;
    memset(bg.legal_targets, 0, sizeof(bg.legal_targets));
    snprintf(bg.status_msg, sizeof(bg.status_msg), "Press [SPACE] or [ENTER] to Roll Dice");
}

static void backgammon_start_dice_roll(void)
{
    int d1 = (int)(esp_random() % 6) + 1;
    int d2 = (int)(esp_random() % 6) + 1;

    if (d1 == d2) {
        bg.dice_count = 4;
        bg.dice[0] = d1;
        bg.dice[1] = d1;
        bg.dice[2] = d1;
        bg.dice[3] = d1;
    } else {
        bg.dice_count = 2;
        bg.dice[0] = d1;
        bg.dice[1] = d2;
        bg.dice[2] = 0;
        bg.dice[3] = 0;
    }

    for (int i = 0; i < 4; i++) {
        bg.dice_used[i] = false;
    }

    bg.roll_anim_frames = 5;
    bg.dice_rolled = true;
    bg.selected_from = -1;
    memset(bg.legal_targets, 0, sizeof(bg.legal_targets));
    bg_sfx_dice_clatter();
}

static void backgammon_finalize_roll(void)
{
    int d1 = bg.dice[0];
    int d2 = bg.dice[1];

    if (!backgammon_has_any_legal_moves(bg.white_turn)) {
        snprintf(bg.status_msg, sizeof(bg.status_msg), "No legal moves possible with [%d-%d]! Turn passed.", d1, d2);
        bg_sfx_pass();
        bg.white_turn = !bg.white_turn;
        bg.dice_rolled = false;
        bg.selected_from = -1;
        memset(bg.legal_targets, 0, sizeof(bg.legal_targets));
        if (!bg.white_turn && bg.vs_cpu) {
            bg.cpu_move_at_us = esp_timer_get_time() + 600000;
        }
        return;
    }

    bool has_bar = (bg.white_turn && bg.bar_white > 0) || (!bg.white_turn && bg.bar_black > 0);
    if (has_bar) {
        backgammon_select_piece(0);
        snprintf(bg.status_msg, sizeof(bg.status_msg), "BAR HIT! Pick target [%s] to re-enter checker.",
                 bg.white_turn ? "19-24" : "1-6");
    } else {
        /* Auto-focus first movable piece */
        backgammon_nav_movable_sources(1);
        if (d1 == d2) {
            snprintf(bg.status_msg, sizeof(bg.status_msg), "%s rolled DOUBLES [%d-%d]! [SPACE/ENTER] Select",
                     bg.white_turn ? "White" : "Black", d1, d2);
        } else {
            snprintf(bg.status_msg, sizeof(bg.status_msg), "%s rolled [%d-%d]. [SPACE/ENTER] Select",
                     bg.white_turn ? "White" : "Black", d1, d2);
        }
    }
}

static bool backgammon_can_bear_off(bool is_white)
{
    if (is_white) {
        if (bg.bar_white > 0) return false;
        for (int i = 7; i <= 24; i++) {
            if (bg.points[i] > 0) return false;
        }
        return true;
    } else {
        if (bg.bar_black > 0) return false;
        for (int i = 1; i <= 18; i++) {
            if (bg.points[i] < 0) return false;
        }
        return true;
    }
}

static bool backgammon_is_legal_move(int from, int die_val, bool is_white, int *out_to)
{
    if (die_val <= 0) return false;

    int target = 0;

    if (is_white) {
        if (from == 0) {
            /* Entering from bar */
            if (bg.bar_white <= 0) return false;
            target = 25 - die_val; /* e.g. die 1 -> point 24, die 6 -> point 19 */
        } else {
            if (bg.bar_white > 0) return false; /* Must enter bar first */
            if (from < 1 || from > 24 || bg.points[from] <= 0) return false;
            target = from - die_val;
        }

        if (target <= 0) {
            /* Bearing off */
            if (!backgammon_can_bear_off(true)) return false;
            if (target == 0) {
                if (out_to) *out_to = 25;
                return true;
            }
            /* Die is larger than needed: legal only if no checkers on higher points */
            for (int p = from + 1; p <= 6; p++) {
                if (bg.points[p] > 0) return false;
            }
            if (out_to) *out_to = 25;
            return true;
        }

        /* Normal board target: cannot land if opponent has 2+ checkers (kapı) */
        if (bg.points[target] < -1) {
            return false;
        }

        if (out_to) *out_to = target;
        return true;
    } else {
        /* Black move */
        if (from == 0) {
            if (bg.bar_black <= 0) return false;
            target = die_val; /* e.g. die 1 -> point 1, die 6 -> point 6 */
        } else {
            if (bg.bar_black > 0) return false;
            if (from < 1 || from > 24 || bg.points[from] >= 0) return false;
            target = from + die_val;
        }

        if (target >= 25) {
            /* Bearing off */
            if (!backgammon_can_bear_off(false)) return false;
            if (target == 25) {
                if (out_to) *out_to = 25;
                return true;
            }
            for (int p = from - 1; p >= 19; p--) {
                if (bg.points[p] < 0) return false;
            }
            if (out_to) *out_to = 25;
            return true;
        }

        if (bg.points[target] > 1) {
            return false;
        }

        if (out_to) *out_to = target;
        return true;
    }
}

static void backgammon_calculate_targets(int from)
{
    memset(bg.legal_targets, 0, sizeof(bg.legal_targets));
    if (from < 0 || from > 24 || !bg.dice_rolled) return;

    for (int i = 0; i < bg.dice_count; i++) {
        if (bg.dice_used[i]) continue;
        int to = 0;
        if (backgammon_is_legal_move(from, bg.dice[i], bg.white_turn, &to)) {
            if (to >= 1 && to <= 25) {
                bg.legal_targets[to] = true;
            }
        }
    }
}

static bool backgammon_point_has_legal_move(int p, bool is_white)
{
    if (!bg.dice_rolled) return false;

    if (p == 0) {
        if (is_white && bg.bar_white <= 0) return false;
        if (!is_white && bg.bar_black <= 0) return false;
    } else {
        if (p < 1 || p > 24) return false;
        if (is_white && (bg.bar_white > 0 || bg.points[p] <= 0)) return false;
        if (!is_white && (bg.bar_black > 0 || bg.points[p] >= 0)) return false;
    }

    for (int i = 0; i < bg.dice_count; i++) {
        if (!bg.dice_used[i] && backgammon_is_legal_move(p, bg.dice[i], is_white, NULL)) {
            return true;
        }
    }
    return false;
}

static bool backgammon_has_any_legal_moves(bool is_white)
{
    if (!bg.dice_rolled) return false;

    bool has_bar = (is_white && bg.bar_white > 0) || (!is_white && bg.bar_black > 0);
    if (has_bar) {
        return backgammon_point_has_legal_move(0, is_white);
    }

    for (int p = 1; p <= 24; p++) {
        if (backgammon_point_has_legal_move(p, is_white)) return true;
    }
    return false;
}

static void backgammon_nav_movable_sources(int dir)
{
    bool is_white = bg.white_turn;
    bool has_bar = (is_white && bg.bar_white > 0) || (!is_white && bg.bar_black > 0);
    if (has_bar) {
        bg.cursor_point = 0;
        return;
    }

    int cur = bg.cursor_point;
    if (cur < 1) cur = 1;
    if (cur > 24) cur = 24;

    for (int step = 1; step <= 24; step++) {
        int next = cur + (dir > 0 ? step : -step);
        while (next < 1) next += 24;
        while (next > 24) next -= 24;
        if (backgammon_point_has_legal_move(next, is_white)) {
            bg.cursor_point = next;
            bg_sfx_tone(1100, 10);
            return;
        }
    }
}

static void backgammon_nav_legal_targets(int dir)
{
    int cur = bg.cursor_point;
    for (int step = 1; step <= 26; step++) {
        int next = cur + (dir > 0 ? step : -step);
        while (next < 1) next += 25;
        while (next > 25) next -= 25;
        if (bg.legal_targets[next]) {
            bg.cursor_point = next;
            bg_sfx_tone(1300, 12);
            return;
        }
    }
}

static void backgammon_select_piece(int p)
{
    bg.selected_from = p;
    backgammon_calculate_targets(p);

    /* Immediately focus the first valid legal target */
    for (int to = 1; to <= 25; to++) {
        if (bg.legal_targets[to]) {
            bg.cursor_point = to;
            break;
        }
    }
    if (p == 0) {
        snprintf(bg.status_msg, sizeof(bg.status_msg), "BAR selected. Pick target. [ESC/BKSP] Cancel");
    } else {
        snprintf(bg.status_msg, sizeof(bg.status_msg), "Point %d selected. Pick target. [ESC/BKSP] Cancel", p);
    }
    bg_sfx_tone(1400, 15);
}

static bool backgammon_apply_move(int from, int to, bool is_white)
{
    int used_die_idx = -1;

    for (int i = 0; i < bg.dice_count; i++) {
        if (bg.dice_used[i]) continue;
        int target = 0;
        if (backgammon_is_legal_move(from, bg.dice[i], is_white, &target)) {
            if (target == to) {
                used_die_idx = i;
                break;
            }
        }
    }

    if (used_die_idx == -1) return false;

    bg.dice_used[used_die_idx] = true;

    /* Remove from source */
    if (from == 0) {
        if (is_white) bg.bar_white--;
        else bg.bar_black--;
    } else {
        if (is_white) bg.points[from]--;
        else bg.points[from]++;
    }

    /* Place at destination */
    if (to == 25) {
        /* Borne off */
        if (is_white) bg.off_white++;
        else bg.off_black++;
        bg_sfx_bear_off();
    } else {
        bool hit = false;
        if (is_white) {
            if (bg.points[to] == -1) {
                bg.points[to] = 0;
                bg.bar_black++;
                hit = true;
                snprintf(bg.status_msg, sizeof(bg.status_msg), "White HIT Black on point %d!", to);
                bg_sfx_hit();
            }
            bg.points[to]++;
        } else {
            if (bg.points[to] == 1) {
                bg.points[to] = 0;
                bg.bar_white++;
                hit = true;
                snprintf(bg.status_msg, sizeof(bg.status_msg), "Black HIT White on point %d!", to);
                bg_sfx_hit();
            }
            bg.points[to]--;
        }
        if (!hit) bg_sfx_move_tap();
    }

    /* Check win condition */
    if (bg.off_white >= 15) {
        bg.game_over = true;
        bg.winner_white = true;
        bg.is_mars = (bg.off_black == 0);
        bg.wins_white += bg.is_mars ? 2 : 1;
        snprintf(bg.status_msg, sizeof(bg.status_msg), "WHITE WINS%s!", bg.is_mars ? " (MARS x2)" : "");
        bg_sfx_win_fanfare();
        return true;
    }
    if (bg.off_black >= 15) {
        bg.game_over = true;
        bg.winner_white = false;
        bg.is_mars = (bg.off_white == 0);
        bg.wins_black += bg.is_mars ? 2 : 1;
        snprintf(bg.status_msg, sizeof(bg.status_msg), "BLACK WINS%s!", bg.is_mars ? " (MARS x2)" : "");
        bg_sfx_win_fanfare();
        return true;
    }

    /* Check if turn is done */
    bool all_used = true;
    for (int i = 0; i < bg.dice_count; i++) {
        if (!bg.dice_used[i]) {
            all_used = false;
            break;
        }
    }

    if (all_used || !backgammon_has_any_legal_moves(is_white)) {
        bg.white_turn = !bg.white_turn;
        bg.dice_rolled = false;
        bg.selected_from = -1;
        memset(bg.legal_targets, 0, sizeof(bg.legal_targets));

        if (!bg.game_over) {
            snprintf(bg.status_msg, sizeof(bg.status_msg), "%s's Turn. Roll dice!",
                     bg.white_turn ? "White" : "Black");
            if (!bg.white_turn && bg.vs_cpu) {
                bg.cpu_move_at_us = esp_timer_get_time() + 450000;
            }
        }
    } else {
        /* Next move in same turn: prepare selection */
        bg.selected_from = -1;
        memset(bg.legal_targets, 0, sizeof(bg.legal_targets));
        bool has_bar = (is_white && bg.bar_white > 0) || (!is_white && bg.bar_black > 0);
        if (has_bar) {
            backgammon_select_piece(0);
        } else {
            backgammon_nav_movable_sources(1);
        }
    }

    return true;
}

/* AI CPU Decision Engine */
static void backgammon_cpu_play_turn(void)
{
    if (bg.white_turn || bg.game_over) return;

    if (!bg.dice_rolled) {
        backgammon_start_dice_roll();
        return;
    }

    if (bg.roll_anim_frames > 0) return;

    if (!backgammon_has_any_legal_moves(false)) {
        bg.white_turn = true;
        bg.dice_rolled = false;
        bg.selected_from = -1;
        memset(bg.legal_targets, 0, sizeof(bg.legal_targets));
        snprintf(bg.status_msg, sizeof(bg.status_msg), "Black has no legal moves. White's Turn!");
        bg_sfx_pass();
        return;
    }

    int best_from = -1;
    int best_to = -1;
    int best_score = -9999;

    if (bg.bar_black > 0) {
        for (int d = 0; d < bg.dice_count; d++) {
            if (bg.dice_used[d]) continue;
            int to = 0;
            if (backgammon_is_legal_move(0, bg.dice[d], false, &to)) {
                int score = 50;
                if (to >= 1 && to <= 24 && bg.points[to] == 1) score += 80;
                if (score > best_score) {
                    best_score = score;
                    best_from = 0;
                    best_to = to;
                }
            }
        }
    } else {
        for (int p = 1; p <= 24; p++) {
            if (bg.points[p] >= 0) continue;
            for (int d = 0; d < bg.dice_count; d++) {
                if (bg.dice_used[d]) continue;
                int to = 0;
                if (backgammon_is_legal_move(p, bg.dice[d], false, &to)) {
                    int score = 10;
                    if (to == 25) {
                        score += 100;
                    } else if (to >= 1 && to <= 24) {
                        if (bg.points[to] == 1) score += 90;
                        if (bg.points[to] == -1) score += 40;
                        if (bg.points[p] == -1) score += 30;
                        score += (to - p);
                    }
                    if (score > best_score) {
                        best_score = score;
                        best_from = p;
                        best_to = to;
                    }
                }
            }
        }
    }

    if (best_from != -1 && best_to != -1) {
        backgammon_apply_move(best_from, best_to, false);
    } else {
        bg.white_turn = true;
        bg.dice_rolled = false;
        bg.selected_from = -1;
        memset(bg.legal_targets, 0, sizeof(bg.legal_targets));
        snprintf(bg.status_msg, sizeof(bg.status_msg), "White's Turn!");
    }
}

static void draw_checker_circle(solar_os_gfx_t *gfx, int cx, int cy, bool is_white)
{
    if (is_white) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_circle(gfx, cx, cy, 7);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_circle(gfx, cx, cy, 5);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_circle(gfx, cx, cy, 3);
    } else {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_circle(gfx, cx, cy, 7);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_circle(gfx, cx, cy, 4);
    }
}

static void draw_down_arrow(solar_os_gfx_t *gfx, int cx, int top_y)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    for (int i = 0; i < 7; i++) {
        solar_os_gfx_fill_rect(gfx, cx - (6 - i), top_y + i, (6 - i) * 2 + 1, 1);
    }
}

static void draw_up_arrow(solar_os_gfx_t *gfx, int cx, int bot_y)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    for (int i = 0; i < 7; i++) {
        solar_os_gfx_fill_rect(gfx, cx - (6 - i), bot_y - i, (6 - i) * 2 + 1, 1);
    }
}

static void draw_vertical_text(solar_os_gfx_t *gfx, int x, int y_start, const char *str, int spacing)
{
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char buf[2] = {0, 0};
    int y = y_start;
    for (int i = 0; str[i] != '\0'; i++) {
        buf[0] = str[i];
        solar_os_gfx_text(gfx, x, y, buf);
        y += spacing;
    }
}

static void backgammon_draw_board(solar_os_gfx_t *gfx)
{
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    /* 1. Header Bar */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 15, "SOLAROS TAVLA (BACKGAMMON)");

    char score_txt[48];
    snprintf(score_txt, sizeof(score_txt), "W: %u  B: %u | %s",
             (unsigned)bg.wins_white, (unsigned)bg.wins_black,
             bg.vs_cpu ? "vs CPU" : "2-Player");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t sw = solar_os_gfx_text_width(gfx, score_txt);
    solar_os_gfx_text(gfx, screen_w - (int)sw - 8, 15, score_txt);

    /* 2. Wooden Board Boundaries */
    const int board_x = 8;
    const int board_y = 25;
    const int board_w = 312;
    const int board_h = 248;
    const int mid_bar_x = board_x + (board_w / 2) - 10;
    const int point_w = 23;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, board_x, board_y, board_w, board_h);
    solar_os_gfx_rect(gfx, board_x + 1, board_y + 1, board_w - 2, board_h - 2);

    /* Middle Wooden Bar */
    solar_os_gfx_fill_rect(gfx, mid_bar_x, board_y, 20, board_h);

    /* 3. Vertical Home Area Labels */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    draw_vertical_text(gfx, 323, board_y + 24, "Black", 15);
    draw_vertical_text(gfx, 323, board_y + 160, "White", 15);

    /* 4. Draw 24 Triangular Points */
    /* Points 13..24 on TOP (13..18 Left, 19..24 Right) */
    for (int i = 0; i < 12; i++) {
        int point_idx = 13 + i;
        int px = (i < 6) ? (board_x + 4 + i * point_w) : (mid_bar_x + 20 + 4 + (i - 6) * point_w);
        int py = board_y + 2;
        int tri_h = 92;

        bool is_dark_point = (i % 2 == 0);
        bool is_cursor = (bg.cursor_point == point_idx);
        bool is_target = bg.legal_targets[point_idx];
        bool is_selected = (bg.selected_from == point_idx);

        if (is_selected) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, px, py, point_w - 1, tri_h);
        } else if (is_target) {
            if (is_cursor && bg.blink_phase) {
                /* Active blinking target destination */
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, px, py, point_w - 1, 14);
                solar_os_gfx_rect(gfx, px, py, point_w - 1, tri_h);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
                draw_down_arrow(gfx, px + (point_w / 2), py + 3);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_rect(gfx, px, py, point_w - 1, tri_h);
                solar_os_gfx_rect(gfx, px + 1, py + 1, point_w - 3, tri_h - 2);
                draw_down_arrow(gfx, px + (point_w / 2), py + 2);
            }
        } else if (is_dark_point) {
            for (int y = 0; y < tri_h; y += 3) {
                int shrink = (y * point_w) / (2 * tri_h);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, px + shrink, py + y, point_w - (shrink * 2) - 1, 1);
            }
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, px, py, point_w - 1, tri_h);
        }

        /* Cursor indicator (when no piece is selected) */
        if (is_cursor && bg.selected_from == -1) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            if (bg.blink_phase) {
                solar_os_gfx_fill_rect(gfx, px + (point_w / 2) - 5, py + 1, 11, 6);
            } else {
                solar_os_gfx_rect(gfx, px + (point_w / 2) - 5, py + 1, 11, 6);
            }
        }

        /* Draw Circular Checkers on Top Points */
        int count = bg.points[point_idx];
        if (count != 0) {
            bool is_white = count > 0;
            int num_checkers = abs(count);
            int draw_max = num_checkers > 5 ? 5 : num_checkers;

            for (int c = 0; c < draw_max; c++) {
                int cy = py + 9 + (c * 16);
                int cx = px + (point_w / 2);
                draw_checker_circle(gfx, cx, cy, is_white);
            }

            if (num_checkers > 5) {
                char cnt_s[4];
                snprintf(cnt_s, sizeof(cnt_s), "%d", num_checkers);
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                solar_os_gfx_set_color(gfx, is_white ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_WHITE);
                solar_os_gfx_text(gfx, px + 7, py + 86, cnt_s);
            }
        }
    }

    /* Points 12..1 on BOTTOM (12..7 Left, 6..1 Right) */
    for (int i = 0; i < 12; i++) {
        int point_idx = 12 - i;
        int px = (i < 6) ? (board_x + 4 + i * point_w) : (mid_bar_x + 20 + 4 + (i - 6) * point_w);
        int tri_h = 92;
        int py = board_y + board_h - tri_h - 2;

        bool is_dark_point = (i % 2 == 1);
        bool is_cursor = (bg.cursor_point == point_idx);
        bool is_target = bg.legal_targets[point_idx];
        bool is_selected = (bg.selected_from == point_idx);

        if (is_selected) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, px, py, point_w - 1, tri_h);
        } else if (is_target) {
            if (is_cursor && bg.blink_phase) {
                /* Active blinking target destination */
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, px, py + tri_h - 14, point_w - 1, 14);
                solar_os_gfx_rect(gfx, px, py, point_w - 1, tri_h);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
                draw_up_arrow(gfx, px + (point_w / 2), py + tri_h - 3);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_rect(gfx, px, py, point_w - 1, tri_h);
                solar_os_gfx_rect(gfx, px + 1, py + 1, point_w - 3, tri_h - 2);
                draw_up_arrow(gfx, px + (point_w / 2), py + tri_h - 2);
            }
        } else if (is_dark_point) {
            for (int y = 0; y < tri_h; y += 3) {
                int shrink = (y * point_w) / (2 * tri_h);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, px + shrink, py + tri_h - y - 1, point_w - (shrink * 2) - 1, 1);
            }
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, px, py, point_w - 1, tri_h);
        }

        /* Cursor indicator (when no piece is selected) */
        if (is_cursor && bg.selected_from == -1) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            if (bg.blink_phase) {
                solar_os_gfx_fill_rect(gfx, px + (point_w / 2) - 5, board_y + board_h - 8, 11, 6);
            } else {
                solar_os_gfx_rect(gfx, px + (point_w / 2) - 5, board_y + board_h - 8, 11, 6);
            }
        }

        /* Draw Circular Checkers on Bottom Points */
        int count = bg.points[point_idx];
        if (count != 0) {
            bool is_white = count > 0;
            int num_checkers = abs(count);
            int draw_max = num_checkers > 5 ? 5 : num_checkers;

            for (int c = 0; c < draw_max; c++) {
                int cy = board_y + board_h - 11 - (c * 16);
                int cx = px + (point_w / 2);
                draw_checker_circle(gfx, cx, cy, is_white);
            }

            if (num_checkers > 5) {
                char cnt_s[4];
                snprintf(cnt_s, sizeof(cnt_s), "%d", num_checkers);
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                solar_os_gfx_set_color(gfx, is_white ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_WHITE);
                solar_os_gfx_text(gfx, px + 7, py + 12, cnt_s);
            }
        }
    }

    /* 5. Middle Bar Checkers (Hit Blots) */
    if (bg.cursor_point == 0 || bg.selected_from == 0) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_rect(gfx, mid_bar_x + 1, board_y + 1, 18, board_h - 2);
        if (bg.blink_phase) {
            solar_os_gfx_rect(gfx, mid_bar_x + 2, board_y + 2, 16, board_h - 4);
        }
    }
    if (bg.bar_white > 0) {
        draw_checker_circle(gfx, mid_bar_x + 10, board_y + 45, true);
        char ws[4]; snprintf(ws, sizeof(ws), "%d", bg.bar_white);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_text(gfx, mid_bar_x + 6, board_y + 66, ws);
    }
    if (bg.bar_black > 0) {
        draw_checker_circle(gfx, mid_bar_x + 10, board_y + board_h - 55, false);
        char bs[4]; snprintf(bs, sizeof(bs), "%d", bg.bar_black);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_text(gfx, mid_bar_x + 6, board_y + board_h - 38, bs);
    }

    /* 6. Right Side: Dice & Tray Panel */
    const int panel_x = 334;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, panel_x, board_y, 60, board_h);

    /* Turn Indicator */
    solar_os_gfx_fill_rect(gfx, panel_x, board_y, 60, 20);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, panel_x + 6, board_y + 14, bg.white_turn ? "WHITE" : "BLACK");

    /* Dice Box */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, panel_x + 14, board_y + 36, "DICE");

    if (bg.dice_rolled) {
        for (int d = 0; d < bg.dice_count; d++) {
            int dy = board_y + 42 + (d * 24);
            bool used = bg.dice_used[d];

            if (used) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
                solar_os_gfx_rect(gfx, panel_x + 16, dy, 26, 20);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, panel_x + 16, dy, 26, 20);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            }

            char dv[4];
            if (bg.roll_anim_frames > 0) {
                snprintf(dv, sizeof(dv), "%d", (int)(esp_random() % 6) + 1);
            } else {
                snprintf(dv, sizeof(dv), "%d", bg.dice[d]);
            }
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, panel_x + 25, dy + 15, dv);
        }
    }

    /* Bearing Off Tray */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, panel_x + 6, board_y + 150, 48, 88);
    if (bg.legal_targets[25]) {
        if (bg.cursor_point == 25 && bg.blink_phase) {
            solar_os_gfx_fill_rect(gfx, panel_x + 6, board_y + 150, 48, 20);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_rect(gfx, panel_x + 4, board_y + 148, 52, 92);
            solar_os_gfx_rect(gfx, panel_x + 2, board_y + 146, 56, 96);
        }
    }
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, panel_x + 8, board_y + 165, "HOME");

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char off_txt[32];
    snprintf(off_txt, sizeof(off_txt), "W: %d/15", bg.off_white);
    solar_os_gfx_text(gfx, panel_x + 8, board_y + 185, off_txt);
    snprintf(off_txt, sizeof(off_txt), "B: %d/15", bg.off_black);
    solar_os_gfx_text(gfx, panel_x + 8, board_y + 205, off_txt);

    /* 7. Footer Status Bar */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 276, screen_w, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 292, bg.status_msg);

    solar_os_gfx_present(gfx);
}

static esp_err_t backgammon_start(solar_os_context_t *ctx)
{
    memset(&bg, 0, sizeof(bg));
    bg.vs_cpu = true;
    backgammon_init_board();
    solar_os_context_set_graphics_active(ctx, true);
    backgammon_draw_board(solar_os_context_gfx(ctx));
    return ESP_OK;
}

static void backgammon_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool backgammon_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);

    if (event->type == SOLAR_OS_EVENT_TICK) {
        /* Handle Dice Rolling Animation */
        if (bg.roll_anim_frames > 0) {
            bg.roll_anim_frames--;
            bg_sfx_dice_clatter();
            if (bg.roll_anim_frames == 0) {
                backgammon_finalize_roll();
            }
            backgammon_draw_board(gfx);
            return true;
        }

        /* Blink phase timer */
        bg.last_blink_tick++;
        if (bg.last_blink_tick >= 5) { /* 250ms toggle */
            bg.last_blink_tick = 0;
            bg.blink_phase = !bg.blink_phase;
            if (bg.dice_rolled && !bg.game_over) {
                backgammon_draw_board(gfx);
            }
        }

        if (!bg.white_turn && bg.vs_cpu && !bg.game_over) {
            const int64_t now = esp_timer_get_time();
            if (bg.cpu_move_at_us != 0 && now >= bg.cpu_move_at_us) {
                bg.cpu_move_at_us = now + 450000;
                backgammon_cpu_play_turn();
                backgammon_draw_board(gfx);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const uint8_t u_ch = (uint8_t)event->data.ch;
        const char ch = event->data.ch;

        /* 1. If a piece is currently selected: ESC, Backspace, Delete, C, or X CANCELS selection */
        if (bg.selected_from != -1) {
            if (u_ch == SOLAR_OS_KEY_ESCAPE || u_ch == 8 || u_ch == 127 || u_ch == SOLAR_OS_KEY_DELETE ||
                ch == 'c' || ch == 'C' || ch == 'x' || ch == 'X') {
                bg.selected_from = -1;
                memset(bg.legal_targets, 0, sizeof(bg.legal_targets));
                backgammon_nav_movable_sources(0);
                snprintf(bg.status_msg, sizeof(bg.status_msg), "Selection canceled. Choose a checker.");
                bg_sfx_tone(600, 15);
                backgammon_draw_board(gfx);
                return true;
            }
        }

        /* 2. Exit app with ESC, Q, or App Exit (only when no piece is selected) */
        if (u_ch == SOLAR_OS_KEY_APP_EXIT || u_ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }

        if (ch == 'r' || ch == 'R') {
            backgammon_init_board();
            backgammon_draw_board(gfx);
            return true;
        }

        if (ch == 'm' || ch == 'M') {
            bg.vs_cpu = !bg.vs_cpu;
            snprintf(bg.status_msg, sizeof(bg.status_msg), "Mode: %s", bg.vs_cpu ? "vs CPU" : "2-Player Pass & Play");
            backgammon_draw_board(gfx);
            return true;
        }

        /* CPU Turn lock */
        if (!bg.white_turn && bg.vs_cpu && !bg.game_over) {
            return true;
        }

        /* Roll Dice */
        if (ch == ' ' && !bg.dice_rolled && !bg.game_over) {
            backgammon_start_dice_roll();
            backgammon_draw_board(gfx);
            return true;
        }

        /* Smart Navigation */
        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A' || ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
            if (bg.selected_from != -1) {
                /* Cycle strictly between legal target points */
                backgammon_nav_legal_targets(-1);
            } else if (bg.dice_rolled) {
                /* Cycle strictly between player's movable pieces */
                backgammon_nav_movable_sources(-1);
            }
            backgammon_draw_board(gfx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D' || ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
            if (bg.selected_from != -1) {
                /* Cycle strictly between legal target points */
                backgammon_nav_legal_targets(1);
            } else if (bg.dice_rolled) {
                /* Cycle strictly between player's movable pieces */
                backgammon_nav_movable_sources(1);
            }
            backgammon_draw_board(gfx);
            return true;
        }

        /* Selection & Action */
        if (ch == '\r' || ch == '\n' || ch == ' ') {
            if (!bg.dice_rolled) {
                backgammon_start_dice_roll();
                backgammon_draw_board(gfx);
                return true;
            }

            if (bg.selected_from == -1) {
                /* Select piece under cursor */
                int p = bg.cursor_point;
                if (backgammon_point_has_legal_move(p, bg.white_turn)) {
                    backgammon_select_piece(p);
                }
            } else {
                /* Move selected piece to current target point */
                int to = bg.cursor_point;
                if (bg.legal_targets[to]) {
                    backgammon_apply_move(bg.selected_from, to, bg.white_turn);
                } else {
                    /* Cancel selection */
                    bg.selected_from = -1;
                    memset(bg.legal_targets, 0, sizeof(bg.legal_targets));
                    backgammon_nav_movable_sources(0);
                }
            }
            backgammon_draw_board(gfx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_backgammon_app = {
    .name = "tavla",
    .summary = "classic 24-point backgammon with AI",
    .flags = 0,
    .start = backgammon_start,
    .stop = backgammon_stop,
    .event = backgammon_event,
    .state_slot = &backgammon_state_ptr,
    .state_size = sizeof(backgammon_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 50U,
    .worker_stack_bytes = BACKGAMMON_STACK_SIZE,
};
