#include "solar_os_appbar.h"

#include <stdio.h>
#include <string.h>

#define APPBAR_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define APPBAR_FOOTER_PAD_X 6
#define APPBAR_FOOTER_GAP 3
#define APPBAR_HEADER_MARGIN 4

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
    bool has_breadcrumb;
    bool breadcrumb_full;      /* true: "Prev | CURRENT | Next"; false: "< CURRENT >" */
    int breadcrumb_x, breadcrumb_w;
    char breadcrumb_text[80];
    size_t prev_index, next_index;
} appbar_header_layout_t;

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

    const size_t tab_count = header->tabs.count;
    if (tab_count < 2 || header->tabs.names == NULL) {
        return;
    }

    size_t active = header->tabs.active_index < tab_count ? header->tabs.active_index : 0;
    out->prev_index = (active + tab_count - 1U) % tab_count;
    out->next_index = (active + 1U) % tab_count;

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    char full_text[80];
    snprintf(full_text, sizeof(full_text), "%s | %s | %s",
             header->tabs.names[out->prev_index],
             header->tabs.names[active],
             header->tabs.names[out->next_index]);
    const int full_w = (int)solar_os_gfx_text_width(gfx, full_text);
    const int min_title_reserved = out->title_x + 40;

    if (width - full_w - APPBAR_HEADER_MARGIN >= min_title_reserved) {
        out->has_breadcrumb = true;
        out->breadcrumb_full = true;
        out->breadcrumb_w = full_w;
        out->breadcrumb_x = width - full_w - APPBAR_HEADER_MARGIN;
        strlcpy(out->breadcrumb_text, full_text, sizeof(out->breadcrumb_text));
        return;
    }

    char fallback_text[80];
    snprintf(fallback_text, sizeof(fallback_text), "< %s >", header->tabs.names[active]);
    const int fallback_w = (int)solar_os_gfx_text_width(gfx, fallback_text);
    if (width - fallback_w - APPBAR_HEADER_MARGIN >= min_title_reserved) {
        out->has_breadcrumb = true;
        out->breadcrumb_full = false;
        out->breadcrumb_w = fallback_w;
        out->breadcrumb_x = width - fallback_w - APPBAR_HEADER_MARGIN;
        strlcpy(out->breadcrumb_text, fallback_text, sizeof(out->breadcrumb_text));
    }
    /* If even the fallback doesn't fit, no breadcrumb is drawn at all --
     * an extremely narrow display with a long title takes priority. */
}

void solar_os_appbar_draw_header(solar_os_gfx_t *gfx, const solar_os_appbar_header_t *header)
{
    if (gfx == NULL || header == NULL) return;

    appbar_header_layout_t layout;
    appbar_layout_header(gfx, header, &layout);
    const int width = (int)solar_os_gfx_width(gfx);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, layout.header_h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    if (layout.has_back) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, layout.back_x + layout.back_w / 3, layout.header_h - layout.header_h / 3, "<");
    }

    if (header->title != NULL && header->title[0] != '\0') {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, layout.title_x, layout.header_h - layout.header_h / 3, header->title);
    }

    if (layout.has_breadcrumb) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, layout.breadcrumb_x, layout.header_h - layout.header_h / 3, layout.breadcrumb_text);
    }

    if (header->status_line != NULL) {
        const int status_h = solar_os_appbar_status_line_height(gfx);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, APPBAR_HEADER_MARGIN, layout.header_h + status_h - status_h / 4, header->status_line);
    }
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

    if (layout.has_breadcrumb && x >= layout.breadcrumb_x && x < layout.breadcrumb_x + layout.breadcrumb_w) {
        const int mid = layout.breadcrumb_x + layout.breadcrumb_w / 2;
        out->kind = (x < mid) ? SOLAR_OS_APPBAR_HIT_TAB_PREV : SOLAR_OS_APPBAR_HIT_TAB_NEXT;
        return true;
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
    }
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
