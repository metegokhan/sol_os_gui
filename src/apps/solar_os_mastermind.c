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
#include "solar_os_appbar.h"

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

    int base; /* symbol count per slot: 6 (Easy), 10 (Medium), 16 (Hard/hex) */
} mastermind_state_t;

static void *mm_state_ptr;
#define mm (*(mastermind_state_t *)mm_state_ptr)

/* Renders a stored 0-based symbol as its display character:
 * base 6 -> '1'..'6', base 10 -> '0'..'9', base 16 -> '0'..'F'. */
static char mm_symbol_char(int base, uint8_t v)
{
    if (base == 6) return (char)('1' + v);
    return "0123456789ABCDEF"[v & 0x0F];
}

/* Parses a typed character into a 0-based symbol for the active base, or -1. */
static int mm_char_to_symbol(int base, char ch)
{
    if (base == 6) {
        return (ch >= '1' && ch <= '6') ? (ch - '1') : -1;
    }
    if (ch >= '0' && ch <= '9') {
        const int v = ch - '0';
        return v < base ? v : -1;
    }
    if (base == 16) {
        const char u = (char)toupper((unsigned char)ch);
        if (u >= 'A' && u <= 'F') return 10 + (u - 'A');
    }
    return -1;
}

static const char *mm_level_name(int base)
{
    return base == 6 ? "Easy" : base == 10 ? "Medium" : "Hard";
}

static const char *mm_symbols_label(int base)
{
    return base == 6 ? "1-6" : base == 10 ? "0-9" : "0-F";
}

static void mm_start_new_game(void)
{
    const int saved_base = mm.base == 6 || mm.base == 10 || mm.base == 16 ? mm.base : 6;
    memset(&mm, 0, sizeof(mm));
    mm.base = saved_base;
    for (int i = 0; i < MM_CODE_LEN; i++) {
        mm.secret[i] = (uint8_t)(esp_random() % (uint32_t)mm.base); /* 0..base-1 */
        mm.current_guess[i] = 0;
    }
    mm.cursor_col = 0;
    mm.attempt_count = 0;
    mm.game_over = false;
    mm.won = false;
    snprintf(mm.status_msg, sizeof(mm.status_msg), "Crack the 4-symbol code (%s)!",
             mm_symbols_label(mm.base));
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
        snprintf(mm.status_msg, sizeof(mm.status_msg), "Code cracked in %d attempts! You win!", mm.attempt_count);
        mm_sfx_win();
    } else if (mm.attempt_count >= MM_MAX_ATTEMPTS) {
        mm.game_over = true;
        mm.won = false;
        snprintf(mm.status_msg, sizeof(mm.status_msg), "Out of attempts! Secret was %c %c %c %c",
                 mm_symbol_char(mm.base, mm.secret[0]), mm_symbol_char(mm.base, mm.secret[1]),
                 mm_symbol_char(mm.base, mm.secret[2]), mm_symbol_char(mm.base, mm.secret[3]));
        mm_sfx_lose();
    } else {
        snprintf(mm.status_msg, sizeof(mm.status_msg), "Attempt %d/10: %d exact, %d partial",
                 mm.attempt_count, exact, partial);
        mm_sfx_submit();
    }
}

/* Builds the footer chips. Submit only makes sense mid-game; Level and New
 * are always available (Level shows the current difficulty). */
static size_t mm_build_footer(solar_os_appbar_shortcut_t *items, size_t max_items)
{
    size_t n = 0;
    if (!mm.game_over && n < max_items) {
        items[n].key = '\r'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Submit");
        n++;
    }
    if (n < max_items) {
        items[n].key = 'g'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Lvl:%s", mm_level_name(mm.base));
        n++;
    }
    if (n < max_items) {
        items[n].key = 'r'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "New");
        n++;
    }
    return n;
}

/* Cycles difficulty 6 -> 10 -> 16 -> 6 and restarts the round. */
static void mm_cycle_level(void)
{
    mm.base = mm.base == 6 ? 10 : mm.base == 10 ? 16 : 6;
    mm_start_new_game();
}

/* Greedily word-wraps text into up to max_lines lines of at most max_w px,
 * drawing them from (x,y) downward. Returns nothing. */
static void mm_draw_wrapped(solar_os_gfx_t *gfx, int x, int y, int max_w,
                            int line_h, int max_lines, const char *text)
{
    char line[64];
    size_t line_len = 0;
    int drawn = 0;
    const char *p = text;
    while (*p != '\0' && drawn < max_lines) {
        /* Grab the next word (including a leading space run). */
        const char *word = p;
        while (*p != '\0' && *p != ' ') p++;
        const size_t wlen = (size_t)(p - word);
        while (*p == ' ') p++; /* swallow spaces after the word */

        char candidate[64];
        if (line_len == 0) {
            const size_t n = wlen < sizeof(candidate) - 1 ? wlen : sizeof(candidate) - 1;
            memcpy(candidate, word, n);
            candidate[n] = '\0';
        } else {
            /* current line + space + word */
            snprintf(candidate, sizeof(candidate), "%.*s %.*s",
                     (int)line_len, line, (int)wlen, word);
        }

        if ((int)solar_os_gfx_text_width(gfx, candidate) <= max_w || line_len == 0) {
            strlcpy(line, candidate, sizeof(line));
            line_len = strlen(line);
        } else {
            solar_os_gfx_text(gfx, x, y + drawn * line_h, line);
            drawn++;
            const size_t n = wlen < sizeof(line) - 1 ? wlen : sizeof(line) - 1;
            memcpy(line, word, n);
            line[n] = '\0';
            line_len = n;
        }
    }
    if (line_len > 0 && drawn < max_lines) {
        solar_os_gfx_text(gfx, x, y + drawn * line_h, line);
    }
}

static void mm_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Shared header bar (status is shown in the right panel, not here, so
     * it can't collide with the board frames just below the bar). */
    solar_os_appbar_header_t header = {0};
    header.title = "Code Breaker";
    header.show_back = true;
    solar_os_appbar_draw_header(gfx, &header);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

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
                char dv[4]; snprintf(dv, sizeof(dv), "%c", mm_symbol_char(mm.base, mm.history_guesses[i][c]));
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

        char cv[4]; snprintf(cv, sizeof(cv), "%c", mm_symbol_char(mm.base, mm.current_guess[c]));
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, cx + 6, cy + 22, cv);
    }

    /* Difficulty + valid symbol range (replaces the removed "(1-6)" hint). */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char lvl_line[40];
    snprintf(lvl_line, sizeof(lvl_line), "Level: %s   Symbols: %s",
             mm_level_name(mm.base), mm_symbols_label(mm.base));
    solar_os_gfx_text(gfx, inp_x + 12, inp_y + 88, lvl_line);

    /* Legend & Clue explanation */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, inp_x + 12, inp_y + 105, "FEEDBACK PEGS:");

    solar_os_gfx_fill_rect(gfx, inp_x + 12, inp_y + 120, 12, 12);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, inp_x + 30, inp_y + 130, "Exact number & spot");

    solar_os_gfx_rect(gfx, inp_x + 12, inp_y + 140, 12, 12);
    solar_os_gfx_text(gfx, inp_x + 30, inp_y + 150, "Right number, wrong spot");

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, inp_x + 12, inp_y + 168, "Tap a slot to pick,");
    solar_os_gfx_text(gfx, inp_x + 12, inp_y + 183, "tap again to change.");

    /* Game status, word-wrapped, right under the hint. */
    mm_draw_wrapped(gfx, inp_x + 12, inp_y + 205, inp_w - 20, 15, 3, mm.status_msg);

    /* 4. Shared footer chips (Submit / Level / New). */
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = mm_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);

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

        solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
        const size_t count = mm_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < count) {
                switch (items[fhit.index].key) {
                case '\r': mm_submit_guess(); break;
                case 'g':  mm_cycle_level(); break;
                default:   mm_start_new_game(); break;
                }
                mm_render(ctx);
            }
            return true;
        }

        /* Tap a guess slot to select it; tap the selected one to cycle symbols.
         * Mirrors mm_render()'s selector geometry (inp_x=248). */
        if (!mm.game_over) {
            const int inp_x = 248;
            const int cy = 26 + 40;               /* inp_y + 40 */
            for (int c = 0; c < 4; c++) {
                const int cx = inp_x + 14 + (c * 30);
                if (px >= cx - 2 && px < cx - 2 + 28 && py >= cy - 2 && py < cy - 2 + 36) {
                    if (c == mm.cursor_col) {
                        mm.current_guess[c] = (uint8_t)(mm.current_guess[c] + 1 < mm.base ? mm.current_guess[c] + 1 : 0);
                    } else {
                        mm.cursor_col = c;
                    }
                    mm_sfx_tick();
                    mm_render(ctx);
                    return true;
                }
            }
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
            mm_start_new_game();
            mm_render(ctx);
            return true;
        }

        if (ch == 'g' || ch == 'G') {
            mm_cycle_level();
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

        /* Direct symbol entry first, so hex letters (a-f) in Hard mode win
         * over the WASD navigation aliases. Returns -1 for non-symbols. */
        {
            const int sym = mm_char_to_symbol(mm.base, ch);
            if (sym >= 0) {
                mm.current_guess[mm.cursor_col] = (uint8_t)sym;
                if (mm.cursor_col + 1 < 4) mm.cursor_col++;
                mm_sfx_tick();
                mm_render(ctx);
                return true;
            }
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
            mm.current_guess[mm.cursor_col] =
                (uint8_t)(mm.current_guess[mm.cursor_col] + 1 < mm.base ? mm.current_guess[mm.cursor_col] + 1 : 0);
            mm_sfx_tick();
            mm_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
            mm.current_guess[mm.cursor_col] =
                (uint8_t)(mm.current_guess[mm.cursor_col] > 0 ? mm.current_guess[mm.cursor_col] - 1 : mm.base - 1);
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
