#include "solar_os_help.h"

#include <string.h>

void solar_os_help_chip(solar_os_appbar_shortcut_t *item)
{
    if (item == NULL) {
        return;
    }
    item->key = 'H';
    item->ctrl = true;
    strlcpy(item->label, "Help", sizeof(item->label));
}

bool solar_os_help_char_opens(char ch)
{
    return ch == '?' || (unsigned char)ch == 0x08U; /* '?' or Ctrl+H */
}

void solar_os_help_draw(solar_os_gfx_t *gfx,
                        const char *title,
                        const char *const *lines,
                        size_t line_count)
{
    if (gfx == NULL) {
        return;
    }

    const int w = (int)solar_os_gfx_width(gfx);
    const int h = (int)solar_os_gfx_height(gfx);
    const int bx = 20;
    const int by = 18;
    const int bw = w - 2 * bx;
    const int bh = h - 2 * by;

    /* Panel with a double border so it reads as a modal over the app. */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, bx, by, bw, bh);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, bx, by, bw, bh);
    solar_os_gfx_rect(gfx, bx + 1, by + 1, bw - 2, bh - 2);

    /* Title bar (inverted). */
    solar_os_gfx_fill_rect(gfx, bx + 2, by + 2, bw - 4, 20);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, bx + 10, by + 16, title != NULL ? title : "Help");

    /* Body lines. */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const int line_h = 15;
    const int max_y = by + bh - 22;
    int ly = by + 38;
    for (size_t i = 0; i < line_count && ly <= max_y; i++) {
        if (lines[i] != NULL && lines[i][0] != '\0') {
            solar_os_gfx_text(gfx, bx + 12, ly, lines[i]);
        }
        ly += line_h;
    }

    /* Close hint pinned to the bottom of the panel. */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, bx + 12, by + bh - 8, "Tap or press any key to close");
}
