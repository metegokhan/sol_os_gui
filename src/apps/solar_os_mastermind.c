#include "solar_os_mastermind.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_random.h"
#include "solar_os.h"
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"

#define MM_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(MM_STACK_SIZE);

static void mm_sfx_tick(void)
{
    solar_os_audio_play_tone(1300, 10, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void mm_sfx_submit(void)
{
    solar_os_audio_play_tone(700, 20, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(950, 30, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void mm_sfx_win(void)
{
    solar_os_audio_play_tone(523, 50, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(659, 50, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(784, 60, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(1046, 140, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void mm_sfx_lose(void)
{
    solar_os_audio_play_tone(350, 60, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(220, 100, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

#define MM_MAX_ATTEMPTS 10
#define MM_CODE_LEN 4

typedef struct {
    uint8_t secret[MM_CODE_LEN];
    uint8_t current_guess[MM_CODE_LEN];
    int cursor_col; /* 0..3 */

    uint8_t history_guesses[MM_MAX_ATTEMPTS][MM_CODE_LEN];
    uint8_t history_exact[MM_MAX_ATTEMPTS];  /* Correct number & position (Black pegs) */
    uint8_t history_partial[MM_MAX_ATTEMPTS];/* Correct number, wrong position (White pegs) */
    int attempt_count;

    bool game_over;
    bool won;
    char status_msg[64];
} mastermind_state_t;

static void *mm_state_ptr;
#define mm (*(mastermind_state_t *)mm_state_ptr)

static void mm_start_new_game(void)
{
    memset(&mm, 0, sizeof(mm));
    for (int i = 0; i < MM_CODE_LEN; i++) {
        mm.secret[i] = (uint8_t)((esp_random() % 6) + 1); /* 1..6 */
        mm.current_guess[i] = 1;
    }
    mm.cursor_col = 0;
    mm.attempt_count = 0;
    mm.game_over = false;
    mm.won = false;
    snprintf(mm.status_msg, sizeof(mm.status_msg), "Crack the 4-digit code (1-6)! [UP/DOWN] Change, [ENTER] Submit");
}

static void mm_submit_guess(void)
{
    if (mm.game_over || mm.attempt_count >= MM_MAX_ATTEMPTS) return;

    int att = mm.attempt_count;
    for (int i = 0; i < MM_CODE_LEN; i++) {
        mm.history_guesses[att][i] = mm.current_guess[i];
    }

    bool secret_used[MM_CODE_LEN] = {false};
    bool guess_used[MM_CODE_LEN] = {false};
    int exact = 0;
    int partial = 0;

    /* 1. Find exact matches */
    for (int i = 0; i < MM_CODE_LEN; i++) {
        if (mm.current_guess[i] == mm.secret[i]) {
            exact++;
            secret_used[i] = true;
            guess_used[i] = true;
        }
    }

    /* 2. Find partial matches */
    for (int i = 0; i < MM_CODE_LEN; i++) {
        if (guess_used[i]) continue;
        for (int j = 0; j < MM_CODE_LEN; j++) {
            if (!secret_used[j] && mm.current_guess[i] == mm.secret[j]) {
                partial++;
                secret_used[j] = true;
                break;
            }
        }
    }

    mm.history_exact[att] = (uint8_t)exact;
    mm.history_partial[att] = (uint8_t)partial;
    mm.attempt_count++;

    if (exact == MM_CODE_LEN) {
        mm.game_over = true;
        mm.won = true;
        snprintf(mm.status_msg, sizeof(mm.status_msg), "★ CODE CRACKED in %d attempts! You Win! [R] Restart", mm.attempt_count);
        mm_sfx_win();
    } else if (mm.attempt_count >= MM_MAX_ATTEMPTS) {
        mm.game_over = true;
        mm.won = false;
        snprintf(mm.status_msg, sizeof(mm.status_msg), "OUT OF ATTEMPTS! Secret was [%d %d %d %d]. [R] Restart",
                 mm.secret[0], mm.secret[1], mm.secret[2], mm.secret[3]);
        mm_sfx_lose();
    } else {
        snprintf(mm.status_msg, sizeof(mm.status_msg), "Attempt %d/10: %d Exact [●], %d Partial [○]. Guess next!",
                 mm.attempt_count, exact, partial);
        mm_sfx_submit();
    }
}

static void mm_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 15, "SOLAROS CODE BREAKER (MASTERMIND)");

    char att_s[32]; snprintf(att_s, sizeof(att_s), "Attempt: %d/10", mm.attempt_count);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t sw = solar_os_gfx_text_width(gfx, att_s);
    solar_os_gfx_text(gfx, screen_w - (int)sw - 8, 15, att_s);

    /* 2. Left Side: History Attempts Board */
    const int hist_x = 10;
    const int hist_y = 26;
    const int hist_w = 230;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, hist_x, hist_y, hist_w, 244);

    for (int i = 0; i < MM_MAX_ATTEMPTS; i++) {
        int ry = hist_y + 4 + (i * 24);
        bool has_guess = (i < mm.attempt_count);

        char num_txt[8]; snprintf(num_txt, sizeof(num_txt), "#%02d", i + 1);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_text(gfx, hist_x + 6, ry + 16, num_txt);

        /* 4 Digit Slots */
        for (int c = 0; c < 4; c++) {
            int cx = hist_x + 36 + (c * 24);
            solar_os_gfx_rect(gfx, cx, ry + 2, 20, 20);
            if (has_guess) {
                char dv[4]; snprintf(dv, sizeof(dv), "%d", mm.history_guesses[i][c]);
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
                solar_os_gfx_text(gfx, cx + 6, ry + 16, dv);
            }
        }

        /* Clue Pegs */
        if (has_guess) {
            int px = hist_x + 138;
            int exact = mm.history_exact[i];
            int part = mm.history_partial[i];

            for (int k = 0; k < 4; k++) {
                int kx = px + (k * 20);
                if (k < exact) {
                    /* Solid Black Peg */
                    solar_os_gfx_fill_rect(gfx, kx, ry + 5, 14, 14);
                } else if (k < (exact + part)) {
                    /* Hollow White Peg */
                    solar_os_gfx_rect(gfx, kx, ry + 5, 14, 14);
                    solar_os_gfx_rect(gfx, kx + 1, ry + 6, 12, 12);
                } else {
                    /* Empty dot */
                    solar_os_gfx_fill_rect(gfx, kx + 5, ry + 10, 4, 4);
                }
            }
        }
    }

    /* 3. Right Side: Current Input & Controls */
    const int inp_x = 248;
    const int inp_y = 26;
    const int inp_w = screen_w - inp_x - 8;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, inp_x, inp_y, inp_w, 244);

    solar_os_gfx_fill_rect(gfx, inp_x, inp_y, inp_w, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, inp_x + 12, inp_y + 17, "YOUR GUESS");

    /* Current 4-digit Selector */
    for (int c = 0; c < 4; c++) {
        int cx = inp_x + 14 + (c * 30);
        int cy = inp_y + 40;
        bool is_sel = (c == mm.cursor_col && !mm.game_over);

        if (is_sel) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, cx - 2, cy - 2, 28, 36);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, cx - 2, cy - 2, 28, 36);
        }

        char cv[4]; snprintf(cv, sizeof(cv), "%d", mm.current_guess[c]);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, cx + 6, cy + 22, cv);
    }

    /* Legend & Clue explanation */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, inp_x + 12, inp_y + 105, "FEEDBACK PEGS:");

    solar_os_gfx_fill_rect(gfx, inp_x + 12, inp_y + 120, 12, 12);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, inp_x + 30, inp_y + 130, "Exact number & spot");

    solar_os_gfx_rect(gfx, inp_x + 12, inp_y + 140, 12, 12);
    solar_os_gfx_text(gfx, inp_x + 30, inp_y + 150, "Right number, wrong spot");

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, inp_x + 12, inp_y + 185, "CONTROLS:");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, inp_x + 12, inp_y + 205, "[LEFT/RIGHT] Select Slot");
    solar_os_gfx_text(gfx, inp_x + 12, inp_y + 222, "[UP/DOWN] Change (1-6)");
    solar_os_gfx_text(gfx, inp_x + 12, inp_y + 239, "[ENTER] Submit Guess");

    /* 4. Footer */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 274, screen_w, 26);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 291, mm.status_msg);

    solar_os_gfx_present(gfx);
}

static esp_err_t mm_start(solar_os_context_t *ctx)
{
    memset(&mm, 0, sizeof(mm));
    mm_start_new_game();
    solar_os_context_set_graphics_active(ctx, true);
    mm_render(ctx);
    return ESP_OK;
}

static void mm_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool mm_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }

        if (ch == 'r' || ch == 'R') {
            mm_start_new_game();
            mm_render(ctx);
            return true;
        }

        if (mm.game_over) {
            if (ch == '\r' || ch == '\n' || ch == ' ') {
                mm_start_new_game();
                mm_render(ctx);
            }
            return true;
        }

        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A') {
            if (mm.cursor_col > 0) mm.cursor_col--;
            mm_sfx_tick();
            mm_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D') {
            if (mm.cursor_col + 1 < 4) mm.cursor_col++;
            mm_sfx_tick();
            mm_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
            if (mm.current_guess[mm.cursor_col] < 6) mm.current_guess[mm.cursor_col]++;
            else mm.current_guess[mm.cursor_col] = 1;
            mm_sfx_tick();
            mm_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
            if (mm.current_guess[mm.cursor_col] > 1) mm.current_guess[mm.cursor_col]--;
            else mm.current_guess[mm.cursor_col] = 6;
            mm_sfx_tick();
            mm_render(ctx);
            return true;
        }

        if (ch >= '1' && ch <= '6') {
            mm.current_guess[mm.cursor_col] = (uint8_t)(ch - '0');
            if (mm.cursor_col + 1 < 4) mm.cursor_col++;
            mm_sfx_tick();
            mm_render(ctx);
            return true;
        }

        if (ch == '\r' || ch == '\n' || ch == ' ') {
            mm_submit_guess();
            mm_render(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_mastermind_app = {
    .name = "codebreaker",
    .summary = "classic 4-peg Code Breaker / Mastermind logic game",
    .flags = 0,
    .start = mm_start,
    .stop = mm_stop,
    .event = mm_event,
    .state_slot = &mm_state_ptr,
    .state_size = sizeof(mastermind_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 50U,
    .worker_stack_bytes = MM_STACK_SIZE,
};
