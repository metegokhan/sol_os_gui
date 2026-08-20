#include "solar_os_appbar.h"

#include <stdio.h>
#include <string.h>

#define APPBAR_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define APPBAR_FOOTER_PAD_X 6
#define APPBAR_FOOTER_GAP 10
#define APPBAR_HEADER_MARGIN 4
#define APPBAR_TAB_SEP_W 10
#define APPBAR_TAB_ARROW_W 14
#define APPBAR_TITLE_MAX 48

int solar_os_appbar_header_height(const solar_os_gfx_t *gfx)
{
    const int height = gfx != NULL ? (int)solar_os_gfx_height(gfx) : 300;
    return APPBAR_CLAMP((int)(height * 0.075f), 16, 26);
}

int solar_os_appbar_status_line_height(const solar_os_gfx_t *gfx)
{
    const int height = gfx != NULL ? (int)solar_os_gfx_height(gfx) : 300;
    return APPBAR_CLAMP((int)(height * 0.045f), 10, 14);
}

int solar_os_appbar_footer_height(const solar_os_gfx_t *gfx)
{
    const int height = gfx != NULL ? (int)solar_os_gfx_height(gfx) : 300;
    return APPBAR_CLAMP((int)(height * 0.06f), 14, 22);
}

static int appbar_back_width(const solar_os_gfx_t *gfx)
{
    const int width = gfx != NULL ? (int)solar_os_gfx_width(gfx) : 400;
    return APPBAR_CLAMP((int)(width * 0.09f), 20, 34);
}

/* ---------------------------------------------------------------------
 * Header layout: computed once, shared by draw + hit-test so their
 * geometry can never drift apart.
 * ------------------------------------------------------------------- */
typedef struct {
    int header_h;
    bool has_back;
    int back_x, back_w;
    int title_x;
    int title_max_w;                        /* clip title to stay clear of the tab half */
    size_t tab_count;
    int tab_x[SOLAR_OS_APPBAR_TAB_MAX];
    int tab_w[SOLAR_OS_APPBAR_TAB_MAX];
    bool tab_visible[SOLAR_OS_APPBAR_TAB_MAX]; /* only entries inside the visible window */
    bool has_prev_arrow;
    int prev_arrow_x, prev_arrow_w;
    size_t prev_arrow_target;               /* tab selected when '<' is tapped */
    bool has_next_arrow;
    int next_arrow_x, next_arrow_w;
    size_t next_arrow_target;               /* tab selected when '>' is tapped */
} appbar_header_layout_t;

/* The header splits into a left half (back + title [+ future icons]) and a
 * right half (the tab strip) once tabs are present -- this is the only way
 * a title and a long tab list can never collide, regardless of tab count.
 * When the fixed-order tab strip is wider than its half, it shows a
 * windowed subset around the active tab with '<'/'>' paging arrows; each
 * arrow is itself a tap target for the adjacent (currently hidden) tab, so
 * apps handle it exactly like any other SOLAR_OS_APPBAR_HIT_TAB_ITEM. */
static void appbar_layout_header(solar_os_gfx_t *gfx,
                                 const solar_os_appbar_header_t *header,
                                 appbar_header_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    if (gfx == NULL || header == NULL) return;

    const int width = (int)solar_os_gfx_width(gfx);
    out->header_h = solar_os_appbar_header_height(gfx);
    out->has_back = header->show_back;
    out->back_w = out->has_back ? appbar_back_width(gfx) : 0;
    out->back_x = 0;
    out->title_x = out->has_back ? out->back_w + APPBAR_HEADER_MARGIN : APPBAR_HEADER_MARGIN;
    out->title_max_w = width - out->title_x - APPBAR_HEADER_MARGIN;

    size_t tab_count = header->tabs.count;
    if (tab_count < 2 || header->tabs.names == NULL) {
        return;
    }
    if (tab_count > SOLAR_OS_APPBAR_TAB_MAX) tab_count = SOLAR_OS_APPBAR_TAB_MAX;
    out->tab_count = tab_count;

    const int right_zone_x = width / 2;
    const int right_zone_w = width - right_zone_x - APPBAR_HEADER_MARGIN;
    out->title_max_w = right_zone_x - out->title_x - APPBAR_HEADER_MARGIN;

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    int seg_w[SOLAR_OS_APPBAR_TAB_MAX];
    int full_w = (int)(tab_count - 1) * APPBAR_TAB_SEP_W;
    for (size_t i = 0; i < tab_count; i++) {
        const int text_w = (int)solar_os_gfx_text_width(gfx, header->tabs.names[i]);
        seg_w[i] = text_w + APPBAR_HEADER_MARGIN * 2;
        full_w += seg_w[i];
    }

    size_t active = header->tabs.active_index < tab_count ? header->tabs.active_index : 0;
    size_t start, end;
    int window_w;

    if (full_w <= right_zone_w) {
        /* Everything fits -- no windowing, no arrows. */
        start = 0;
        end = tab_count - 1;
        window_w = full_w;
    } else {
        /* Fixed left-to-right pages (computed independently of which tab is
         * active, never re-centered on it) so that tapping a tab already on
         * screen only moves the highlight -- it never reshuffles the strip.
         * Only the '<'/'>' arrows advance to the adjacent page. */
        const int avail = right_zone_w - 2 * (APPBAR_TAB_ARROW_W + APPBAR_TAB_SEP_W);
        size_t page_start = 0;
        for (;;) {
            size_t page_end = page_start;
            int w = seg_w[page_start];
            while (page_end + 1 < tab_count) {
                const int grown = w + APPBAR_TAB_SEP_W + seg_w[page_end + 1];
                if (grown > avail) break;
                page_end++;
                w = grown;
            }
            if ((active >= page_start && active <= page_end) || page_end + 1 >= tab_count) {
                start = page_start;
                end = page_end;
                window_w = w;
                break;
            }
            page_start = page_end + 1;
        }
    }

    out->has_prev_arrow = start > 0;
    out->has_next_arrow = end < tab_count - 1;
    out->prev_arrow_target = out->has_prev_arrow ? start - 1 : 0;
    out->next_arrow_target = out->has_next_arrow ? end + 1 : 0;

    int block_w = window_w;
    if (out->has_prev_arrow) block_w += APPBAR_TAB_ARROW_W + APPBAR_TAB_SEP_W;
    if (out->has_next_arrow) block_w += APPBAR_TAB_ARROW_W + APPBAR_TAB_SEP_W;

    int x = width - block_w - APPBAR_HEADER_MARGIN;
    if (x < right_zone_x) x = right_zone_x;

    if (out->has_prev_arrow) {
        out->prev_arrow_x = x;
        out->prev_arrow_w = APPBAR_TAB_ARROW_W;
        x += APPBAR_TAB_ARROW_W + APPBAR_TAB_SEP_W;
    }
    for (size_t i = start; i <= end; i++) {
        out->tab_x[i] = x;
        out->tab_w[i] = seg_w[i];
        out->tab_visible[i] = true;
        x += seg_w[i] + APPBAR_TAB_SEP_W;
    }
    if (out->has_next_arrow) {
        out->next_arrow_x = x;
        out->next_arrow_w = APPBAR_TAB_ARROW_W;
    }
}

/* Truncates a copy of `text` (adding an ellipsis) until it fits max_w,
 * then draws it -- used for the title, which now shares the header with
 * a tab strip and can no longer assume the full display width. */
static void appbar_draw_clipped_title(solar_os_gfx_t *gfx, int x, int baseline_y,
                                      const char *text, int max_w)
{
    if (text == NULL || text[0] == '\0' || max_w <= 0) return;

    char buf[APPBAR_TITLE_MAX];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    if ((int)solar_os_gfx_text_width(gfx, buf) <= max_w) {
        solar_os_gfx_text(gfx, x, baseline_y, buf);
        return;
    }

    size_t len = strlen(buf);
    while (len > 0) {
        len--;
        buf[len] = '\0';
        char candidate[APPBAR_TITLE_MAX];
        snprintf(candidate, sizeof(candidate), "%s..", buf);
        if ((int)solar_os_gfx_text_width(gfx, candidate) <= max_w) {
            solar_os_gfx_text(gfx, x, baseline_y, candidate);
            return;
        }
    }
    solar_os_gfx_text(gfx, x, baseline_y, "..");
}

void solar_os_appbar_draw_header(solar_os_gfx_t *gfx, const solar_os_appbar_header_t *header)
{
    if (gfx == NULL || header == NULL) return;

    appbar_header_layout_t layout;
    appbar_layout_header(gfx, header, &layout);
    const int width = (int)solar_os_gfx_width(gfx);
    const size_t active = header->tabs.active_index < header->tabs.count ? header->tabs.active_index : 0;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, layout.header_h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    if (layout.has_back) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, layout.back_x + layout.back_w / 3, layout.header_h - layout.header_h / 3, "<");
    }

    if (header->title != NULL && header->title[0] != '\0') {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        appbar_draw_clipped_title(gfx, layout.title_x, layout.header_h - layout.header_h / 3,
                                  header->title, layout.title_max_w);
    }

    const int text_y = layout.header_h - layout.header_h / 3;

    if (layout.has_prev_arrow) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, layout.prev_arrow_x + 2, text_y, "<");
    }

    size_t prev_visible = SIZE_MAX;
    for (size_t i = 0; i < layout.tab_count; i++) {
        if (!layout.tab_visible[i]) continue;
        const bool is_active = (i == active);
        if (is_active) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_rect(gfx, layout.tab_x[i], 2, layout.tab_w[i], layout.header_h - 4);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        }
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, layout.tab_x[i] + APPBAR_HEADER_MARGIN, text_y, header->tabs.names[i]);

        if (prev_visible != SIZE_MAX) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            const int sep_x = layout.tab_x[prev_visible] + layout.tab_w[prev_visible];
            solar_os_gfx_text(gfx, sep_x + APPBAR_TAB_SEP_W / 2 - 2, text_y, "|");
        }
        prev_visible = i;
    }

    if (layout.has_next_arrow) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, layout.next_arrow_x + 2, text_y, ">");
    }

    if (header->status_line != NULL) {
        const int status_h = solar_os_appbar_status_line_height(gfx);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, APPBAR_HEADER_MARGIN, layout.header_h + status_h - status_h / 4, header->status_line);
    }

    /* Leave the draw color at the conventional default (black on white)
     * before returning -- callers that draw their body immediately after
     * the header, without setting their own color first, have always been
     * able to assume this (the old hand-rolled headers every app used to
     * draw all incidentally ended on black too). The tab strip above is
     * the one thing in this function that legitimately needs white, so
     * without this the active-tab highlight or a "no more tabs on this
     * side" state could leave color on white and make the caller's very
     * next shape invisible (white-on-white). */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
}

bool solar_os_appbar_hit_test_header(const solar_os_gfx_t *gfx,
                                     const solar_os_appbar_header_t *header,
                                     int16_t x, int16_t y,
                                     solar_os_appbar_hit_t *out)
{
    if (gfx == NULL || header == NULL || out == NULL) return false;
    out->kind = SOLAR_OS_APPBAR_HIT_NONE;
    out->index = 0;

    appbar_header_layout_t layout;
    appbar_layout_header((solar_os_gfx_t *)gfx, header, &layout);

    if (y < 0 || y >= layout.header_h) return false;

    if (layout.has_back && x >= layout.back_x && x < layout.back_x + layout.back_w) {
        out->kind = SOLAR_OS_APPBAR_HIT_BACK;
        return true;
    }

    if (layout.has_prev_arrow && x >= layout.prev_arrow_x && x < layout.prev_arrow_x + layout.prev_arrow_w) {
        out->kind = SOLAR_OS_APPBAR_HIT_TAB_ITEM;
        out->index = layout.prev_arrow_target;
        return true;
    }
    if (layout.has_next_arrow && x >= layout.next_arrow_x && x < layout.next_arrow_x + layout.next_arrow_w) {
        out->kind = SOLAR_OS_APPBAR_HIT_TAB_ITEM;
        out->index = layout.next_arrow_target;
        return true;
    }

    for (size_t i = 0; i < layout.tab_count; i++) {
        if (!layout.tab_visible[i]) continue;
        if (x >= layout.tab_x[i] && x < layout.tab_x[i] + layout.tab_w[i]) {
            out->kind = SOLAR_OS_APPBAR_HIT_TAB_ITEM;
            out->index = i;
            return true;
        }
    }

    return true; /* inside the header bar, but no actionable element -- still "handled" so apps don't fall through to body hit-testing */
}

/* ---------------------------------------------------------------------
 * Footer layout: same shared-computation rule as the header.
 * ------------------------------------------------------------------- */
typedef struct {
    int footer_h;
    int top;
    int x[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    int w[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    char text[SOLAR_OS_APPBAR_SHORTCUT_MAX][SOLAR_OS_APPBAR_LABEL_MAX + 8];
    size_t count;
} appbar_footer_layout_t;

static void appbar_layout_footer(solar_os_gfx_t *gfx,
                                 const solar_os_appbar_shortcuts_t *shortcuts,
                                 appbar_footer_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    if (gfx == NULL) return;

    out->footer_h = solar_os_appbar_footer_height(gfx);
    out->top = (int)solar_os_gfx_height(gfx) - out->footer_h;

    if (shortcuts == NULL || shortcuts->items == NULL) return;

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    size_t count = shortcuts->count;
    if (count > SOLAR_OS_APPBAR_SHORTCUT_MAX) count = SOLAR_OS_APPBAR_SHORTCUT_MAX;

    int cursor_x = APPBAR_FOOTER_GAP;
    for (size_t i = 0; i < count; i++) {
        const solar_os_appbar_shortcut_t *item = &shortcuts->items[i];
        if (item->ctrl && item->key != 0) {
            snprintf(out->text[i], sizeof(out->text[i]), "Ctrl+%c %s", item->key, item->label);
        } else {
            snprintf(out->text[i], sizeof(out->text[i]), "%s", item->label);
        }
        const int text_w = (int)solar_os_gfx_text_width(gfx, out->text[i]);
        out->x[i] = cursor_x;
        out->w[i] = text_w + APPBAR_FOOTER_PAD_X * 2;
        cursor_x += out->w[i] + APPBAR_FOOTER_GAP;
    }
    out->count = count;
}

void solar_os_appbar_draw_footer(solar_os_gfx_t *gfx, const solar_os_appbar_shortcuts_t *shortcuts)
{
    if (gfx == NULL) return;
    if (shortcuts == NULL || shortcuts->count == 0) return;

    appbar_footer_layout_t layout;
    appbar_layout_footer(gfx, shortcuts, &layout);
    const int width = (int)solar_os_gfx_width(gfx);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, layout.top, width, layout.footer_h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    for (size_t i = 0; i < layout.count; i++) {
        if (layout.x[i] >= width) break;
        solar_os_gfx_text(gfx, layout.x[i] + APPBAR_FOOTER_PAD_X,
                          layout.top + layout.footer_h - layout.footer_h / 4,
                          layout.text[i]);

        if (i + 1 < layout.count) {
            const int sep_x = layout.x[i] + layout.w[i];
            if (sep_x + APPBAR_FOOTER_GAP <= width) {
                solar_os_gfx_text(gfx, sep_x + APPBAR_FOOTER_GAP / 2 - 2,
                                  layout.top + layout.footer_h - layout.footer_h / 4, "|");
            }
        }
    }

    /* Same reasoning as the end of solar_os_appbar_draw_header(): leave
     * the conventional black-on-white default behind for any caller that
     * draws more without setting its own color first. */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
}

bool solar_os_appbar_hit_test_footer(const solar_os_gfx_t *gfx,
                                     const solar_os_appbar_shortcuts_t *shortcuts,
                                     int16_t x, int16_t y,
                                     solar_os_appbar_hit_t *out)
{
    if (gfx == NULL || out == NULL) return false;
    out->kind = SOLAR_OS_APPBAR_HIT_NONE;
    out->index = 0;
    if (shortcuts == NULL || shortcuts->count == 0) return false;

    appbar_footer_layout_t layout;
    appbar_layout_footer((solar_os_gfx_t *)gfx, shortcuts, &layout);

    if (y < layout.top || y >= layout.top + layout.footer_h) return false;

    for (size_t i = 0; i < layout.count; i++) {
        if (x >= layout.x[i] && x < layout.x[i] + layout.w[i]) {
            out->kind = SOLAR_OS_APPBAR_HIT_FOOTER_ITEM;
            out->index = i;
            return true;
        }
    }
    return true; /* inside the footer bar, but not on a chip */
}
