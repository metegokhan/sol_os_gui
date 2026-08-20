#include "solar_os_dice.h"

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
#include "solar_os_appbar.h"
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"

#define DICE_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(DICE_STACK_SIZE);

#define YATZY_CATEGORIES 13

static void ytz_sfx_roll(void)
{
    solar_os_audio_play_tone(800 + (esp_random() % 600), 15, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void ytz_sfx_hold(void)
{
    solar_os_audio_play_tone(1200, 15, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void ytz_sfx_score(void)
{
    solar_os_audio_play_tone(700, 30, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(1050, 45, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void ytz_sfx_yatzy_fanfare(void)
{
    solar_os_audio_play_tone(523, 50, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(659, 50, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(784, 60, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(1046, 150, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

typedef struct {
    int dice[5];
    bool held[5];
    int rolls_left; /* 3 down to 0 */

    int roll_anim_frames;

    int scores[YATZY_CATEGORIES];
    bool scored[YATZY_CATEGORIES];
    int selected_cat; /* 0 to 12 */
    int cursor_focus; /* 0 = Dice hold area (0..4), 1 = Category scorecard */
    int dice_cursor;  /* 0..4 */

    int total_score;
    bool game_over;
    char status_msg[64];
} yatzy_state_t;

static void *dice_state_ptr;
#define ytz (*(yatzy_state_t *)dice_state_ptr)

static const char *cat_names[YATZY_CATEGORIES] = {
    "1. Ones", "2. Twos", "3. Threes", "4. Fours", "5. Fives", "6. Sixes",
    "7. 3 of Kind", "8. 4 of Kind", "9. Full House", "10. S-Straight",
    "11. L-Straight", "12. YATZY (50)", "13. Chance"
};

static void yatzy_reset_game(void)
{
    memset(&ytz, 0, sizeof(ytz));
    for (int i = 0; i < 5; i++) {
        ytz.dice[i] = (int)(esp_random() % 6) + 1;
        ytz.held[i] = false;
    }
    ytz.rolls_left = 3;
    ytz.roll_anim_frames = 0;
    ytz.selected_cat = 0;
    ytz.cursor_focus = 0;
    ytz.dice_cursor = 0;
    snprintf(ytz.status_msg, sizeof(ytz.status_msg), "Press [SPACE] to Roll 5 Dice (3 rolls left)");
}

static void yatzy_start_roll(void)
{
    if (ytz.rolls_left <= 0) return;
    ytz.roll_anim_frames = 5;
    ytz.rolls_left--;
    ytz_sfx_roll();
}

static void yatzy_finish_roll(void)
{
    for (int i = 0; i < 5; i++) {
        if (!ytz.held[i]) {
            ytz.dice[i] = (int)(esp_random() % 6) + 1;
        }
    }
    snprintf(ytz.status_msg, sizeof(ytz.status_msg), "Rolled! %d rolls remaining. [1..5] Hold | [DOWN] Scorecard", ytz.rolls_left);
}

static int yatzy_calculate_potential_score(int cat, const int dice[5])
{
    int counts[7] = {0};
    int sum = 0;
    for (int i = 0; i < 5; i++) {
        counts[dice[i]]++;
        sum += dice[i];
    }

    if (cat >= 0 && cat <= 5) {
        /* Upper section (Ones through Sixes) */
        int target = cat + 1;
        return counts[target] * target;
    }

    switch (cat) {
    case 6: { /* 3 of a Kind */
        for (int i = 1; i <= 6; i++) {
            if (counts[i] >= 3) return sum;
        }
        return 0;
    }
    case 7: { /* 4 of a Kind */
        for (int i = 1; i <= 6; i++) {
            if (counts[i] >= 4) return sum;
        }
        return 0;
    }
    case 8: { /* Full House */
        bool has3 = false, has2 = false;
        for (int i = 1; i <= 6; i++) {
            if (counts[i] == 3) has3 = true;
            if (counts[i] == 2) has2 = true;
        }
        return (has3 && has2) ? 25 : 0;
    }
    case 9: { /* Small Straight (4 in a row) */
        if ((counts[1] && counts[2] && counts[3] && counts[4]) ||
            (counts[2] && counts[3] && counts[4] && counts[5]) ||
            (counts[3] && counts[4] && counts[5] && counts[6])) {
            return 30;
        }
        return 0;
    }
    case 10: { /* Large Straight (5 in a row) */
        if ((counts[1] && counts[2] && counts[3] && counts[4] && counts[5]) ||
            (counts[2] && counts[3] && counts[4] && counts[5] && counts[6])) {
            return 40;
        }
        return 0;
    }
    case 11: { /* Yatzy (5 of a kind) */
        for (int i = 1; i <= 6; i++) {
            if (counts[i] == 5) return 50;
        }
        return 0;
    }
    case 12: { /* Chance */
        return sum;
    }
    default:
        return 0;
    }
}

static void yatzy_apply_score(int cat)
{
    if (cat < 0 || cat >= YATZY_CATEGORIES || ytz.scored[cat]) return;

    ytz.scores[cat] = yatzy_calculate_potential_score(cat, ytz.dice);
    ytz.scored[cat] = true;

    if (cat == 11 && ytz.scores[cat] == 50) {
        ytz_sfx_yatzy_fanfare();
    } else {
        ytz_sfx_score();
    }

    /* Check bonus */
    int upper_sum = 0;
    for (int i = 0; i < 6; i++) {
        if (ytz.scored[i]) upper_sum += ytz.scores[i];
    }
    int total = 0;
    for (int i = 0; i < YATZY_CATEGORIES; i++) {
        if (ytz.scored[i]) total += ytz.scores[i];
    }
    if (upper_sum >= 63) total += 35; /* Bonus */
    ytz.total_score = total;

    /* Check game end */
    bool all_done = true;
    for (int i = 0; i < YATZY_CATEGORIES; i++) {
        if (!ytz.scored[i]) {
            all_done = false;
            break;
        }
    }

    if (all_done) {
        ytz.game_over = true;
        snprintf(ytz.status_msg, sizeof(ytz.status_msg), "GAME OVER! Final Score: %d points! [R] Restart", ytz.total_score);
    } else {
        /* New turn */
        for (int i = 0; i < 5; i++) ytz.held[i] = false;
        ytz.rolls_left = 3;
        ytz.cursor_focus = 0;
        yatzy_start_roll();
    }
}

static void yatzy_draw_dice_pip_box(solar_os_gfx_t *gfx, int x, int y, int size, int val, bool is_held, bool is_cursor)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, x, y, size, size);
    solar_os_gfx_set_color(gfx, is_held ? SOLAR_OS_GFX_COLOR_DARK : SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x + 2, y + 2, size - 4, size - 4);

    solar_os_gfx_set_color(gfx, is_held ? SOLAR_OS_GFX_COLOR_WHITE : SOLAR_OS_GFX_COLOR_BLACK);

    /* Draw pips */
    int mid = x + size / 2;
    int mid_y = y + size / 2;
    int q1_x = x + 10, q1_y = y + 10;
    int q2_x = x + size - 11, q2_y = y + size - 11;
    int pip_s = 4;

    if (val == 1 || val == 3 || val == 5) {
        solar_os_gfx_fill_rect(gfx, mid - 2, mid_y - 2, pip_s, pip_s);
    }
    if (val >= 2) {
        solar_os_gfx_fill_rect(gfx, q1_x - 2, q1_y - 2, pip_s, pip_s);
        solar_os_gfx_fill_rect(gfx, q2_x - 2, q2_y - 2, pip_s, pip_s);
    }
    if (val >= 4) {
        solar_os_gfx_fill_rect(gfx, q2_x - 2, q1_y - 2, pip_s, pip_s);
        solar_os_gfx_fill_rect(gfx, q1_x - 2, q2_y - 2, pip_s, pip_s);
    }
    if (val == 6) {
        solar_os_gfx_fill_rect(gfx, q1_x - 2, mid_y - 2, pip_s, pip_s);
        solar_os_gfx_fill_rect(gfx, q2_x - 2, mid_y - 2, pip_s, pip_s);
    }

    if (is_cursor) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, x - 2, y - 2, size + 4, size + 4);
    }
}

static void yatzy_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header (no status_line -- the scorecard below is already packed
     * tight against the footer, so the score goes in the title instead). */
    char title_txt[40];
    snprintf(title_txt, sizeof(title_txt), "Yatzy - Score: %d", ytz.total_score);
    solar_os_appbar_header_t header = {0};
    header.title = title_txt;
    header.show_back = true;
    solar_os_appbar_draw_header(gfx, &header);

    /* 2. Left Side: Scorecard Table */
    const int table_x = 8;
    const int table_y = 26;
    const int table_w = 175;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, table_x, table_y, table_w, 244);

    for (int i = 0; i < YATZY_CATEGORIES; i++) {
        int ry = table_y + 4 + (i * 18);
        bool is_sel = (ytz.cursor_focus == 1 && ytz.selected_cat == i);

        if (is_sel) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, table_x + 2, ry, table_w - 4, 17);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, table_x + 6, ry + 13, cat_names[i]);

        char val_s[16];
        if (ytz.scored[i]) {
            snprintf(val_s, sizeof(val_s), "%d", ytz.scores[i]);
        } else {
            int pot = yatzy_calculate_potential_score(i, ytz.dice);
            snprintf(val_s, sizeof(val_s), "(%d)", pot);
        }
        const size_t vw = solar_os_gfx_text_width(gfx, val_s);
        solar_os_gfx_text(gfx, table_x + table_w - (int)vw - 6, ry + 13, val_s);
    }

    /* 3. Right Side: 5 Dice Box Area */
    const int dice_box_x = 190;
    const int dice_box_y = 26;
    const int dice_box_w = screen_w - dice_box_x - 8;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, dice_box_x, dice_box_y, dice_box_w, 244);

    solar_os_gfx_fill_rect(gfx, dice_box_x, dice_box_y, dice_box_w, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char roll_info[48]; snprintf(roll_info, sizeof(roll_info), "ROLLS LEFT: %d / 3", ytz.rolls_left);
    solar_os_gfx_text(gfx, dice_box_x + 12, dice_box_y + 17, roll_info);

    /* Draw 5 Dice */
    for (int i = 0; i < 5; i++) {
        int dx = dice_box_x + 16 + ((i % 3) * 58);
        int dy = dice_box_y + 36 + ((i / 3) * 62);
        bool is_cursor = (ytz.cursor_focus == 0 && ytz.dice_cursor == i);
        int d_val = ytz.dice[i];
        if (ytz.roll_anim_frames > 0 && !ytz.held[i]) {
            d_val = (int)(esp_random() % 6) + 1;
        }
        yatzy_draw_dice_pip_box(gfx, dx, dy, 46, d_val, ytz.held[i], is_cursor);

        if (ytz.held[i]) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, dx + 8, dy + 56, "[HELD]");
        }
    }

    /* Help text */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, dice_box_x + 12, dice_box_y + 185, "[1..5] Toggle Hold Dice");
    solar_os_gfx_text(gfx, dice_box_x + 12, dice_box_y + 205, "[SPACE] Roll Unheld Dice");
    solar_os_gfx_text(gfx, dice_box_x + 12, dice_box_y + 225, "[LEFT/RIGHT] Switch Focus");

    /* 4. Footer */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 274, screen_w, 26);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 291, ytz.status_msg);

    solar_os_gfx_present(gfx);
}

static esp_err_t yatzy_start(solar_os_context_t *ctx)
{
    memset(&ytz, 0, sizeof(ytz));
    yatzy_reset_game();
    solar_os_context_set_graphics_active(ctx, true);
    yatzy_render(ctx);
    return ESP_OK;
}

static void yatzy_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool yatzy_event(solar_os_context_t *ctx, const solar_os_event_t *event)
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

        /* Mirrors yatzy_render()'s scorecard/dice-box geometry. */
        const int table_x = 8, table_y = 26, table_w = 175;
        const int dice_box_x = 190, dice_box_y = 26;
        const int screen_w = (int)solar_os_gfx_width(gfx);
        const int dice_box_w = screen_w - dice_box_x - 8;

        if (px >= table_x && px < table_x + table_w && py >= table_y + 4) {
            const int row = (py - (table_y + 4)) / 18;
            if (row >= 0 && row < YATZY_CATEGORIES) {
                ytz.cursor_focus = 1;
                ytz.selected_cat = row;
                if (!ytz.scored[row]) {
                    yatzy_apply_score(row);
                }
                yatzy_render(ctx);
            }
            return true;
        }

        if (px >= dice_box_x && px < dice_box_x + dice_box_w &&
            py >= dice_box_y + 36 && py < dice_box_y + 36 + 2 * 62) {
            const int col = (px - (dice_box_x + 16)) / 58;
            const int drow = (py - (dice_box_y + 36)) / 62;
            const int d = drow * 3 + col;
            if (col >= 0 && col < 3 && drow >= 0 && drow < 2 && d < 5) {
                ytz.cursor_focus = 0;
                ytz.dice_cursor = d;
                ytz.held[d] = !ytz.held[d];
                ytz_sfx_hold();
                yatzy_render(ctx);
            }
            return true;
        }

        if (px >= dice_box_x && px < dice_box_x + dice_box_w &&
            py >= dice_box_y && py < dice_box_y + 24) {
            if (ytz.rolls_left > 0) {
                yatzy_start_roll();
                yatzy_render(ctx);
            }
            return true;
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (ytz.roll_anim_frames > 0) {
            ytz.roll_anim_frames--;
            ytz_sfx_roll();
            if (ytz.roll_anim_frames == 0) {
                yatzy_finish_roll();
            }
            yatzy_render(ctx);
            return true;
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }

        if (ch == 'r' || ch == 'R') {
            yatzy_reset_game();
            yatzy_render(ctx);
            return true;
        }

        if (ch >= '1' && ch <= '5') {
            int d = ch - '1';
            ytz.held[d] = !ytz.held[d];
            ytz_sfx_hold();
            yatzy_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A') {
            ytz.cursor_focus = 1; /* Focus scorecard */
            yatzy_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D') {
            ytz.cursor_focus = 0; /* Focus dice */
            yatzy_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
            if (ytz.cursor_focus == 1) {
                if (ytz.selected_cat > 0) ytz.selected_cat--;
            } else {
                if (ytz.dice_cursor > 0) ytz.dice_cursor--;
            }
            yatzy_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
            if (ytz.cursor_focus == 1) {
                if (ytz.selected_cat + 1 < YATZY_CATEGORIES) ytz.selected_cat++;
            } else {
                if (ytz.dice_cursor + 1 < 5) ytz.dice_cursor++;
            }
            yatzy_render(ctx);
            return true;
        }

        if (ch == ' ') {
            if (ytz.rolls_left > 0) {
                yatzy_start_roll();
            } else {
                ytz.cursor_focus = 1;
            }
            yatzy_render(ctx);
            return true;
        }

        if (ch == '\r' || ch == '\n') {
            if (ytz.cursor_focus == 0) {
                /* Toggle hold under cursor */
                ytz.held[ytz.dice_cursor] = !ytz.held[ytz.dice_cursor];
                ytz_sfx_hold();
            } else {
                /* Apply score to category */
                if (!ytz.scored[ytz.selected_cat]) {
                    yatzy_apply_score(ytz.selected_cat);
                }
            }
            yatzy_render(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_dice_app = {
    .name = "yatzy",
    .summary = "classic 5-dice Yatzy strategy game",
    .flags = 0,
    .start = yatzy_start,
    .stop = yatzy_stop,
    .event = yatzy_event,
    .state_slot = &dice_state_ptr,
    .state_size = sizeof(yatzy_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 50U,
    .worker_stack_bytes = DICE_STACK_SIZE,
};
