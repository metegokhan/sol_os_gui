#include "solar_os_sudoku.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"
#include "solar_os_appbar.h"
#include "solar_os_help.h"

#define SUDOKU_STACK_SIZE 12288
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(SUDOKU_STACK_SIZE);

#define SUD_MAX 9

typedef enum { SUD_BEGINNER = 0, SUD_STANDARD = 1, SUD_HARD = 2 } sud_diff_t;

typedef struct {
    int n;                       /* board size: 4, 6 or 9 */
    int br, bc;                  /* box height, box width */
    uint8_t grid[SUD_MAX][SUD_MAX];
    uint8_t solution[SUD_MAX][SUD_MAX];
    bool initial[SUD_MAX][SUD_MAX];
    uint16_t marks[SUD_MAX][SUD_MAX]; /* pencil candidates, bit (v-1) */

    int cursor_r, cursor_c;
    sud_diff_t diff;
    int hints_left;
    bool notes_mode;

    bool picker_open;
    int picker_r, picker_c;

    bool solved;
    bool show_help;
    char status_msg[64];
} sudoku_state_t;

static void *sudoku_state_ptr;
#define sudoku (*(sudoku_state_t *)sudoku_state_ptr)

static const char *const sud_diff_names[] = { "Beginner", "Standard", "Hard" };

static const char *const sud_help_lines[] = {
    "Fill the grid so every row, column and box holds",
    "each number exactly once.",
    "",
    "Play:",
    "  - Tap a cell: a number ring appears; tap a number",
    "    to place it, or the center dot to clear.",
    "  - Notes: toggle pencil mode, then pick numbers in",
    "    the ring to jot candidates (small corner digits).",
    "  - Hint: fills the selected cell (3 per puzzle).",
    "  - Duplicates in a row/column/box show inverted.",
    "",
    "Size cycles 4x4 / 6x6 / 9x9; Level sets difficulty.",
    "Keys: arrows move, 1-9 place, 0/Del clear, Tab notes,",
    "  T hint, Z size, V level, R new, Esc exit.",
};
#define SUD_HELP_LINE_COUNT (sizeof(sud_help_lines) / sizeof(sud_help_lines[0]))

/* ------------------------------------------------------------------ model */

static void sud_box_dims(int n, int *br, int *bc)
{
    if (n == 4) { *br = 2; *bc = 2; }
    else if (n == 6) { *br = 2; *bc = 3; }
    else { *br = 3; *bc = 3; }
}

static bool sud_valid(const uint8_t g[SUD_MAX][SUD_MAX], int n, int br, int bc,
                      int r, int c, uint8_t v)
{
    for (int i = 0; i < n; i++) {
        if (g[r][i] == v || g[i][c] == v) return false;
    }
    const int r0 = (r / br) * br;
    const int c0 = (c / bc) * bc;
    for (int dr = 0; dr < br; dr++) {
        for (int dc = 0; dc < bc; dc++) {
            if (g[r0 + dr][c0 + dc] == v) return false;
        }
    }
    return true;
}

/* Recursive randomized fill of a complete valid grid. */
static bool sud_fill(uint8_t g[SUD_MAX][SUD_MAX], int n, int br, int bc, int pos)
{
    if (pos == n * n) return true;
    const int r = pos / n;
    const int c = pos % n;

    uint8_t order[SUD_MAX];
    for (int i = 0; i < n; i++) order[i] = (uint8_t)(i + 1);
    for (int i = n - 1; i > 0; i--) {
        const int j = (int)(esp_random() % (uint32_t)(i + 1));
        const uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
    }

    for (int i = 0; i < n; i++) {
        const uint8_t v = order[i];
        if (sud_valid(g, n, br, bc, r, c, v)) {
            g[r][c] = v;
            if (sud_fill(g, n, br, bc, pos + 1)) return true;
            g[r][c] = 0;
        }
    }
    return false;
}

static void sud_generate(void)
{
    const int n = sudoku.n;
    sud_box_dims(n, &sudoku.br, &sudoku.bc);

    memset(sudoku.grid, 0, sizeof(sudoku.grid));
    memset(sudoku.marks, 0, sizeof(sudoku.marks));
    (void)sud_fill(sudoku.grid, n, sudoku.br, sudoku.bc, 0);
    memcpy(sudoku.solution, sudoku.grid, sizeof(sudoku.solution));

    /* Clues to keep, by difficulty (fraction of cells). */
    const float frac = sudoku.diff == SUD_BEGINNER ? 0.60f :
                       sudoku.diff == SUD_STANDARD ? 0.48f : 0.36f;
    const int total = n * n;
    int keep = (int)(frac * (float)total + 0.5f);
    if (keep < n) keep = n;
    int remove = total - keep;

    /* Clear `remove` distinct random cells. */
    while (remove > 0) {
        const int r = (int)(esp_random() % (uint32_t)n);
        const int c = (int)(esp_random() % (uint32_t)n);
        if (sudoku.grid[r][c] != 0) {
            sudoku.grid[r][c] = 0;
            remove--;
        }
    }

    for (int r = 0; r < n; r++) {
        for (int c = 0; c < n; c++) {
            sudoku.initial[r][c] = (sudoku.grid[r][c] != 0);
        }
    }

    sudoku.cursor_r = n / 2;
    sudoku.cursor_c = n / 2;
    sudoku.solved = false;
    sudoku.picker_open = false;
    sudoku.hints_left = 3;
    snprintf(sudoku.status_msg, sizeof(sudoku.status_msg), "%s  %dx%d",
             sud_diff_names[sudoku.diff], n, n);
}

/* True if the filled cell (r,c) duplicates its value in row/col/box. */
static bool sud_is_conflict(int r, int c)
{
    const int n = sudoku.n;
    const uint8_t v = sudoku.grid[r][c];
    if (v == 0) return false;
    for (int i = 0; i < n; i++) {
        if (i != c && sudoku.grid[r][i] == v) return true;
        if (i != r && sudoku.grid[i][c] == v) return true;
    }
    const int r0 = (r / sudoku.br) * sudoku.br;
    const int c0 = (c / sudoku.bc) * sudoku.bc;
    for (int dr = 0; dr < sudoku.br; dr++) {
        for (int dc = 0; dc < sudoku.bc; dc++) {
            const int rr = r0 + dr, cc = c0 + dc;
            if ((rr != r || cc != c) && sudoku.grid[rr][cc] == v) return true;
        }
    }
    return false;
}

static void sud_check_win(void)
{
    const int n = sudoku.n;
    for (int r = 0; r < n; r++) {
        for (int c = 0; c < n; c++) {
            if (sudoku.grid[r][c] == 0 || sudoku.grid[r][c] != sudoku.solution[r][c]) {
                return;
            }
        }
    }
    sudoku.solved = true;
    strlcpy(sudoku.status_msg, "Solved! Well done.", sizeof(sudoku.status_msg));
}

/* Places a value (or 0 to clear) in the current cell, respecting givens. */
static void sud_place(int r, int c, uint8_t v)
{
    if (sudoku.initial[r][c]) return;
    sudoku.grid[r][c] = v;
    sudoku.marks[r][c] = 0;
    if (v == 0) sudoku.solved = false;
    else sud_check_win();
}

static void sud_toggle_mark(int r, int c, uint8_t v)
{
    if (sudoku.initial[r][c] || sudoku.grid[r][c] != 0 || v < 1 || v > sudoku.n) return;
    sudoku.marks[r][c] ^= (uint16_t)(1u << (v - 1));
}

static void sud_hint(void)
{
    const int r = sudoku.cursor_r, c = sudoku.cursor_c;
    if (sudoku.hints_left <= 0) {
        strlcpy(sudoku.status_msg, "No hints left.", sizeof(sudoku.status_msg));
        return;
    }
    if (sudoku.initial[r][c] || sudoku.grid[r][c] == sudoku.solution[r][c]) {
        strlcpy(sudoku.status_msg, "Pick an empty cell for a hint.", sizeof(sudoku.status_msg));
        return;
    }
    sudoku.grid[r][c] = sudoku.solution[r][c];
    sudoku.marks[r][c] = 0;
    sudoku.hints_left--;
    snprintf(sudoku.status_msg, sizeof(sudoku.status_msg), "Hint used. %d left.", sudoku.hints_left);
    sud_check_win();
}

/* ------------------------------------------------------------------ layout */

#define SUD_BOARD_PX 238

static int sud_cell_px(void) { return SUD_BOARD_PX / sudoku.n; }

static int sud_board_top(solar_os_gfx_t *gfx)
{
    return solar_os_appbar_header_height(gfx) + solar_os_appbar_status_line_height(gfx) + 4;
}

static const int SUD_BOARD_X = 12;

/* Radial picker geometry: ring centre (clamped on-screen) + radius. */
static void sud_picker_center(solar_os_gfx_t *gfx, int *cx, int *cy, int *R)
{
    const int cell = sud_cell_px();
    const int gy = sud_board_top(gfx);
    int x = SUD_BOARD_X + sudoku.picker_c * cell + cell / 2;
    int y = gy + sudoku.picker_r * cell + cell / 2;
    const int rad = 34 + sudoku.n * 2;
    const int w = (int)solar_os_gfx_width(gfx);
    const int h = (int)solar_os_gfx_height(gfx);
    const int m = rad + 14;
    if (x < m) x = m; else if (x > w - m) x = w - m;
    if (y < m + 20) y = m + 20; else if (y > h - m - 20) y = h - m - 20;
    *cx = x; *cy = y; *R = rad;
}

static void sud_bubble_pos(int i, int cx, int cy, int R, int *bx, int *by)
{
    const float a = -1.5707963f + (2.0f * 3.14159265f * (float)i) / (float)sudoku.n;
    *bx = cx + (int)((float)R * cosf(a));
    *by = cy + (int)((float)R * sinf(a));
}

/* ------------------------------------------------------------------ footer */

static size_t sud_build_footer(solar_os_appbar_shortcut_t *items, size_t max)
{
    size_t n = 0;
    if (n < max) { items[n].key = 't'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Hint (%d)", sudoku.hints_left); n++; }
    if (n < max) { items[n].key = 'p'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), sudoku.notes_mode ? "Notes:On" : "Notes:Off"); n++; }
    if (n < max) { items[n].key = 'r'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "New"); n++; }
    if (n < max) { items[n].key = 'z'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Size:%d", sudoku.n); n++; }
    if (n < max) { items[n].key = 'v'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Lvl:%.4s", sud_diff_names[sudoku.diff]); n++; }
    if (n < max) { solar_os_help_chip(&items[n]); n++; }
    return n;
}

/* ------------------------------------------------------------------ render */

static void sud_draw_number(solar_os_gfx_t *gfx, int cx, int cy, int cell, uint8_t v, bool bold)
{
    char s[4];
    snprintf(s, sizeof(s), "%d", v);
    solar_os_gfx_font_t f = cell >= 48 ? SOLAR_OS_GFX_FONT_BOLD_20 :
                            cell >= 34 ? SOLAR_OS_GFX_FONT_BOLD_14 :
                            bold ? SOLAR_OS_GFX_FONT_BOLD : SOLAR_OS_GFX_FONT_SMALL;
    solar_os_gfx_set_font(gfx, f);
    const int tw = (int)solar_os_gfx_text_width(gfx, s);
    solar_os_gfx_text(gfx, cx + (cell - tw) / 2, cy + (cell * 2) / 3, s);
}

static void sud_draw_marks(solar_os_gfx_t *gfx, int cx, int cy, int cell, uint16_t marks)
{
    if (marks == 0 || cell < 22) return;
    const int mcols = sudoku.n <= 4 ? 2 : 3;
    const int mrows = (sudoku.n + mcols - 1) / mcols;
    const int sw = cell / mcols;
    const int sh = cell / mrows;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    for (int v = 1; v <= sudoku.n; v++) {
        if (!(marks & (uint16_t)(1u << (v - 1)))) continue;
        const int idx = v - 1;
        const int col = idx % mcols;
        const int row = idx / mcols;
        char s[3]; snprintf(s, sizeof(s), "%d", v);
        solar_os_gfx_text(gfx, cx + col * sw + 2, cy + row * sh + sh - 2, s);
    }
}

static void sudoku_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* Header */
    solar_os_appbar_header_t header = {0};
    header.title = "Sudoku";
    header.show_back = true;
    char status_line[64];
    snprintf(status_line, sizeof(status_line), "%s   %dx%d   Hints: %d%s",
             sud_diff_names[sudoku.diff], sudoku.n, sudoku.n, sudoku.hints_left,
             sudoku.notes_mode ? "   [Notes]" : "");
    header.status_line = status_line;
    solar_os_appbar_draw_header(gfx, &header);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    const int n = sudoku.n;
    const int cell = sud_cell_px();
    const int gx = SUD_BOARD_X;
    const int gy = sud_board_top(gfx);
    const int bpx = cell * n;

    /* Cells */
    for (int r = 0; r < n; r++) {
        for (int c = 0; c < n; c++) {
            const int cx = gx + c * cell;
            const int cy = gy + r * cell;
            const uint8_t val = sudoku.grid[r][c];
            const bool conflict = sud_is_conflict(r, c);

            if (conflict) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, cx, cy, cell, cell);
            }
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, cx, cy, cell, cell);

            if (r == sudoku.cursor_r && c == sudoku.cursor_c) {
                solar_os_gfx_rect(gfx, cx + 1, cy + 1, cell - 2, cell - 2);
                solar_os_gfx_rect(gfx, cx + 2, cy + 2, cell - 4, cell - 4);
            }

            if (val != 0) {
                solar_os_gfx_set_color(gfx, conflict ? SOLAR_OS_GFX_COLOR_WHITE : SOLAR_OS_GFX_COLOR_BLACK);
                sud_draw_number(gfx, cx, cy, cell, val, sudoku.initial[r][c]);
            } else {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                sud_draw_marks(gfx, cx, cy, cell, sudoku.marks[r][c]);
            }
        }
    }

    /* Thick box borders */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    for (int bx = 0; bx <= n; bx += sudoku.bc) {
        solar_os_gfx_line(gfx, gx + bx * cell, gy, gx + bx * cell, gy + bpx);
        solar_os_gfx_line(gfx, gx + bx * cell + 1, gy, gx + bx * cell + 1, gy + bpx);
    }
    for (int by = 0; by <= n; by += sudoku.br) {
        solar_os_gfx_line(gfx, gx, gy + by * cell, gx + bpx, gy + by * cell);
        solar_os_gfx_line(gfx, gx, gy + by * cell + 1, gx + bpx, gy + by * cell + 1);
    }

    /* Right side panel */
    const int panel_x = gx + bpx + 10;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    if (panel_x < screen_w - 20) {
        solar_os_gfx_text(gfx, panel_x, gy + 14, sudoku.status_msg);
        if (sudoku.solved) {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, panel_x, gy + 44, "SOLVED!");
        }
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, panel_x, gy + 74, "Tap a cell to");
        solar_os_gfx_text(gfx, panel_x, gy + 88, "open the number");
        solar_os_gfx_text(gfx, panel_x, gy + 102, "ring.");
        if (sudoku.notes_mode) {
            solar_os_gfx_text(gfx, panel_x, gy + 128, "Notes mode ON:");
            solar_os_gfx_text(gfx, panel_x, gy + 142, "ring picks jot");
            solar_os_gfx_text(gfx, panel_x, gy + 156, "candidates.");
        }
    }

    /* Footer chips */
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = sud_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);

    /* Radial number picker overlay */
    if (sudoku.picker_open) {
        int pcx, pcy, R;
        sud_picker_center(gfx, &pcx, &pcy, &R);
        /* faint guide ring */
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        for (int i = 0; i < n; i++) {
            int bx, by;
            sud_bubble_pos(i, pcx, pcy, R, &bx, &by);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_circle(gfx, bx, by, 13);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_circle(gfx, bx, by, 13);
            const bool marked = (sudoku.grid[sudoku.picker_r][sudoku.picker_c] == 0) &&
                (sudoku.marks[sudoku.picker_r][sudoku.picker_c] & (uint16_t)(1u << i)) != 0;
            if (marked) solar_os_gfx_circle(gfx, bx, by, 11);
            char s[3]; snprintf(s, sizeof(s), "%d", i + 1);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            const int tw = (int)solar_os_gfx_text_width(gfx, s);
            solar_os_gfx_text(gfx, bx - tw / 2, by + 5, s);
        }
        /* center clear button */
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_circle(gfx, pcx, pcy, 13);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_circle(gfx, pcx, pcy, 13);
        solar_os_gfx_line(gfx, pcx - 5, pcy - 5, pcx + 5, pcy + 5);
        solar_os_gfx_line(gfx, pcx - 5, pcy + 5, pcx + 5, pcy - 5);
    }

    /* Help overlay */
    if (sudoku.show_help) {
        solar_os_help_draw(gfx, "Sudoku - Help", sud_help_lines, SUD_HELP_LINE_COUNT);
    }

    solar_os_gfx_present(gfx);
}

/* ------------------------------------------------------------------ lifecycle */

static esp_err_t sudoku_start(solar_os_context_t *ctx)
{
    memset(&sudoku, 0, sizeof(sudoku));
    sudoku.n = 9;
    sudoku.diff = SUD_STANDARD;
    sud_generate();
    solar_os_context_set_graphics_active(ctx, true);
    sudoku_render(ctx);
    return ESP_OK;
}

static void sudoku_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

/* ------------------------------------------------------------------ input */

static void sud_apply_pick(uint8_t v) /* v in 1..n, or 0 to clear */
{
    const int r = sudoku.picker_r, c = sudoku.picker_c;
    if (v == 0) {
        sud_place(r, c, 0);
    } else if (sudoku.notes_mode) {
        sud_toggle_mark(r, c, v);
    } else {
        sud_place(r, c, v);
    }
}

static bool sudoku_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        if (sudoku.show_help) {
            sudoku.show_help = false;
            sudoku_render(ctx);
            return true;
        }

        /* Radial picker takes priority when open. */
        if (sudoku.picker_open) {
            int pcx, pcy, R;
            sud_picker_center(gfx, &pcx, &pcy, &R);
            /* center clear */
            if ((px - pcx) * (px - pcx) + (py - pcy) * (py - pcy) <= 13 * 13) {
                sud_apply_pick(0);
                sudoku.picker_open = false;
                sudoku_render(ctx);
                return true;
            }
            for (int i = 0; i < sudoku.n; i++) {
                int bx, by;
                sud_bubble_pos(i, pcx, pcy, R, &bx, &by);
                if ((px - bx) * (px - bx) + (py - by) * (py - by) <= 14 * 14) {
                    sud_apply_pick((uint8_t)(i + 1));
                    if (!sudoku.notes_mode) sudoku.picker_open = false;
                    sudoku_render(ctx);
                    return true;
                }
            }
            /* tap elsewhere closes */
            sudoku.picker_open = false;
            sudoku_render(ctx);
            return true;
        }

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
        const size_t count = sud_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < count) {
                switch (items[fhit.index].key) {
                case 't': sud_hint(); break;
                case 'p': sudoku.notes_mode = !sudoku.notes_mode; break;
                case 'r': sud_generate(); break;
                case 'z': sudoku.n = sudoku.n == 9 ? 4 : sudoku.n == 4 ? 6 : 9; sud_generate(); break;
                case 'v': sudoku.diff = (sud_diff_t)((sudoku.diff + 1) % 3); sud_generate(); break;
                case 'H': sudoku.show_help = true; break;
                default: break;
                }
                sudoku_render(ctx);
            }
            return true;
        }

        /* Board cell tap -> select + open the ring. */
        const int cell = sud_cell_px();
        const int gy = sud_board_top(gfx);
        const int bpx = cell * sudoku.n;
        if (px >= SUD_BOARD_X && px < SUD_BOARD_X + bpx && py >= gy && py < gy + bpx) {
            const int c = (px - SUD_BOARD_X) / cell;
            const int r = (py - gy) / cell;
            if (r >= 0 && r < sudoku.n && c >= 0 && c < sudoku.n) {
                sudoku.cursor_r = r;
                sudoku.cursor_c = c;
                if (!sudoku.initial[r][c]) {
                    sudoku.picker_r = r;
                    sudoku.picker_c = c;
                    sudoku.picker_open = true;
                }
                sudoku_render(ctx);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;
        const int n = sudoku.n;

        if (sudoku.show_help) {
            sudoku.show_help = false;
            sudoku_render(ctx);
            return true;
        }
        if (solar_os_help_char_opens(ch)) {
            sudoku.show_help = true;
            sudoku_render(ctx);
            return true;
        }
        if (sudoku.picker_open && (ch == SOLAR_OS_KEY_ESCAPE || ch == ' ')) {
            sudoku.picker_open = false;
            sudoku_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W' || ch == 'k' || ch == 'K') {
            if (sudoku.cursor_r > 0) sudoku.cursor_r--;
            sudoku_render(ctx); return true;
        }
        if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S' || ch == 'j' || ch == 'J') {
            if (sudoku.cursor_r < n - 1) sudoku.cursor_r++;
            sudoku_render(ctx); return true;
        }
        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A') {
            if (sudoku.cursor_c > 0) sudoku.cursor_c--;
            sudoku_render(ctx); return true;
        }
        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D') {
            if (sudoku.cursor_c < n - 1) sudoku.cursor_c++;
            sudoku_render(ctx); return true;
        }

        if (ch >= '1' && (ch - '0') <= n) {
            const uint8_t v = (uint8_t)(ch - '0');
            if (sudoku.notes_mode) sud_toggle_mark(sudoku.cursor_r, sudoku.cursor_c, v);
            else sud_place(sudoku.cursor_r, sudoku.cursor_c, v);
            sudoku_render(ctx);
            return true;
        }
        if (ch == '0' || ch == '\b' || ch == 127 || ch == SOLAR_OS_KEY_DELETE) {
            sud_place(sudoku.cursor_r, sudoku.cursor_c, 0);
            sudoku_render(ctx);
            return true;
        }

        if (ch == 't' || ch == 'T') { sud_hint(); sudoku_render(ctx); return true; }
        if (ch == 'p' || ch == 'P' || ch == '\t') { sudoku.notes_mode = !sudoku.notes_mode; sudoku_render(ctx); return true; }
        if (ch == 'r' || ch == 'R') { sud_generate(); sudoku_render(ctx); return true; }
        if (ch == 'z' || ch == 'Z') { sudoku.n = sudoku.n == 9 ? 4 : sudoku.n == 4 ? 6 : 9; sud_generate(); sudoku_render(ctx); return true; }
        if (ch == 'v' || ch == 'V') { sudoku.diff = (sud_diff_t)((sudoku.diff + 1) % 3); sud_generate(); sudoku_render(ctx); return true; }
        if (ch == 'n' || ch == 'N') { sud_generate(); sudoku_render(ctx); return true; }

        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_sudoku_app = {
    .name = "sudoku",
    .summary = "Sudoku with 4x4/6x6/9x9 sizes, hints and pencil marks",
    .flags = 0,
    .start = sudoku_start,
    .stop = sudoku_stop,
    .event = sudoku_event,
    .state_slot = &sudoku_state_ptr,
    .state_size = sizeof(sudoku_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = SUDOKU_STACK_SIZE,
};
