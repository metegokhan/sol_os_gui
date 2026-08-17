/*
 * Solar OS - Shared App Chrome (header/footer bar)
 *
 * A resolution-proportional, clickable replacement for every app's own
 * hand-drawn header/footer bar. Apps that adopt this stop defining their own
 * *_HEADER_H/*_FOOTER_H pixel constants and stop hand-typing shortcut
 * strings; instead they describe *what* the bar should show (title, tabs,
 * shortcuts) and this component handles layout, drawing, and hit-testing.
 *
 * Footer convention: only list shortcuts that are NOT obvious (Enter, Esc,
 * arrow-key navigation are never included -- every app already relies on
 * those universally, so they don't need explaining). A shortcut's label is
 * its function only ("Save", "Connect"), never the raw key, except Ctrl
 * combos, which render as "Ctrl+<KEY> <Label>" since the modifier isn't
 * otherwise discoverable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "solar_os_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_APPBAR_SHORTCUT_MAX 6U
#define SOLAR_OS_APPBAR_LABEL_MAX 20U

typedef struct {
    char key;                                  /* triggering char, e.g. 'S' */
    bool ctrl;                                  /* true -> footer shows "Ctrl+S Save" */
    char label[SOLAR_OS_APPBAR_LABEL_MAX];      /* function name only, e.g. "Save" */
} solar_os_appbar_shortcut_t;

typedef struct {
    const solar_os_appbar_shortcut_t *items;    /* caller-owned static array */
    size_t count;                                /* <= SOLAR_OS_APPBAR_SHORTCUT_MAX */
} solar_os_appbar_shortcuts_t;

typedef struct {
    const char * const *names;    /* e.g. {"Info","Settings","GATT","Radar"} */
    size_t count;                  /* 0 or 1 => no breadcrumb drawn */
    size_t active_index;
} solar_os_appbar_tabs_t;

typedef struct {
    const char *title;
    bool show_back;                 /* false only for the launcher/home screen */
    solar_os_appbar_tabs_t tabs;    /* zero-init (count=0) if the app has no tabs */
    const char *status_line;        /* optional 2nd slim line below the bar; NULL = none */
} solar_os_appbar_header_t;

typedef enum {
    SOLAR_OS_APPBAR_HIT_NONE,
    SOLAR_OS_APPBAR_HIT_BACK,
    SOLAR_OS_APPBAR_HIT_TAB_PREV,
    SOLAR_OS_APPBAR_HIT_TAB_NEXT,
    SOLAR_OS_APPBAR_HIT_FOOTER_ITEM,   /* see .index */
} solar_os_appbar_hit_kind_t;

typedef struct {
    solar_os_appbar_hit_kind_t kind;
    size_t index;                       /* valid when kind == FOOTER_ITEM */
} solar_os_appbar_hit_t;

/* Proportional layout queries -- use these instead of a fixed pixel
 * constant so the bar scales with the actual display resolution. */
int solar_os_appbar_header_height(const solar_os_gfx_t *gfx);
int solar_os_appbar_status_line_height(const solar_os_gfx_t *gfx);
int solar_os_appbar_footer_height(const solar_os_gfx_t *gfx);

/* Draws the header bar (and its status line, if header->status_line is
 * non-NULL) at y=0. Callers that use status_line must reserve
 * header_height(gfx) + status_line_height(gfx) for their own body-content
 * top offset on every frame, even when that frame's status text happens to
 * be empty, so the layout never jitters between frames. */
void solar_os_appbar_draw_header(solar_os_gfx_t *gfx, const solar_os_appbar_header_t *header);

/* Draws the footer bar anchored to the bottom of the display. */
void solar_os_appbar_draw_footer(solar_os_gfx_t *gfx, const solar_os_appbar_shortcuts_t *shortcuts);

/* Both hit-test functions use the exact same layout math as their
 * corresponding draw function (shared private helpers in the .c file) --
 * never hand-duplicate this geometry in an app. */
bool solar_os_appbar_hit_test_header(const solar_os_gfx_t *gfx,
                                     const solar_os_appbar_header_t *header,
                                     int16_t x, int16_t y,
                                     solar_os_appbar_hit_t *out);
bool solar_os_appbar_hit_test_footer(const solar_os_gfx_t *gfx,
                                     const solar_os_appbar_shortcuts_t *shortcuts,
                                     int16_t x, int16_t y,
                                     solar_os_appbar_hit_t *out);

#ifdef __cplusplus
}
#endif
