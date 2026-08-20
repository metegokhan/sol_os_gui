/*
 * Solar OS - Shared Help Overlay
 *
 * A standard "Ctrl+H Help" affordance every graphical app can adopt so the
 * user always has one consistent way to see an app's shortcuts and usage.
 *
 * Usage:
 *   1. Add the Help chip to your footer: solar_os_help_chip(&items[n++]).
 *   2. Keep a `bool show_help` flag in your state.
 *   3. Open it when the Help chip is tapped, or when a CHAR event satisfies
 *      solar_os_help_char_opens(ch).
 *   4. In your render, after drawing the normal screen but before present,
 *      if show_help draw solar_os_help_draw(gfx, title, lines, count).
 *   5. Any click or key press while show_help is set closes it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "solar_os_gfx.h"
#include "solar_os_appbar.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fills `item` with the standard Help chip ("Ctrl+H Help"). */
void solar_os_help_chip(solar_os_appbar_shortcut_t *item);

/* True if `ch` should open the help overlay: '?' or Ctrl+H (0x08). */
bool solar_os_help_char_opens(char ch);

/* Draws a centered modal help panel: `title` bar over `line_count` text
 * lines, plus a "tap/press to close" hint. Caller draws its own screen
 * first, calls this, then presents. Lines that overflow the panel are
 * dropped. */
void solar_os_help_draw(solar_os_gfx_t *gfx,
                        const char *title,
                        const char *const *lines,
                        size_t line_count);

#ifdef __cplusplus
}
#endif
