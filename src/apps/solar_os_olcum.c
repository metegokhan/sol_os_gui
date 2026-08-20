#include "solar_os_olcum.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_appbar.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_storage.h"

#define TAG "olcum"

#define OLCUM_PI 3.14159265358979323846f

/* Physical active screen dimensions (mm), as specified by the manufacturer */
#define OLCUM_SCREEN_MM_W 84.60f
#define OLCUM_SCREEN_MM_H 63.60f
#define OLCUM_PX_PER_MM_X (400.0f / OLCUM_SCREEN_MM_W)
#define OLCUM_PX_PER_MM_Y (300.0f / OLCUM_SCREEN_MM_H)

/* Matches solar_os_appbar_header_height()/footer_height() for this board's
 * resolution -- kept as plain constants here since the ruler/circle/
 * protractor math below addresses fixed pixel origins, not the proportional
 * appbar API, but the shared appbar component draws the actual bars. */
#define OLCUM_HEADER_H 22
#define OLCUM_FOOTER_H 18

/* Calibration: lets the user compare the drawn scale against a real
 * ruler and fine-tune px/mm, then persist it to storage. This is a
 * safety net around the manufacturer-specified OLCUM_SCREEN_MM_W/H
 * above -- useful for unit-to-unit manufacturing tolerance, and
 * especially important if this app is ever ported to a different
 * display/board where the panel size constants above no longer apply.
 * (A previous edit of this file dropped this view and the persistence
 * code entirely, most likely by accident rather than a deliberate
 * removal -- restored here.) */
#define OLCUM_CALIBRATION_FILE "calibration.cfg"
#define OLCUM_CALIBRATION_MAGIC 0x4F4C4331U /* "OLC1" */
#define OLCUM_CALIBRATION_VERSION 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    float px_per_mm_x;
    float px_per_mm_y;
} olcum_calibration_t;

typedef enum {
    OLCUM_VIEW_RULER = 0,
    OLCUM_VIEW_CIRCLES = 1,
    OLCUM_VIEW_PROTRACTOR = 2,
    OLCUM_VIEW_CALIBRATION = 3,
    OLCUM_VIEW_COUNT = 4,
} olcum_view_t;

typedef struct {
    olcum_view_t view;
    float px_per_mm_x;
    float px_per_mm_y;
    float angle_deg;

    uint32_t elapsed_ms;
    bool render_pending;
    char status_message[64];
    uint32_t status_until_ms;
} olcum_state_t;

static void *olcum_state_ptr;
#define olcum (*(olcum_state_t *)olcum_state_ptr)

typedef struct {
    const char *label;
    float mm;
} olcum_screw_t;

static const olcum_screw_t OLCUM_SCREWS[] = {
    {"M1.5", 1.5f}, {"M2", 2.0f}, {"M2.5", 2.5f}, {"M3", 3.0f}, {"M4", 4.0f}, {"M5", 5.0f},
};
#define OLCUM_SCREW_COUNT (sizeof(OLCUM_SCREWS) / sizeof(OLCUM_SCREWS[0]))

static void olcum_render(solar_os_context_t *ctx);

/* One-shot status line (no periodic tick in this app, so there is no
 * timed expiry -- the message is shown for exactly the next render and
 * cleared immediately after). */
static void olcum_set_status(const char *message)
{
    strncpy(olcum.status_message, message, sizeof(olcum.status_message) - 1U);
    olcum.status_message[sizeof(olcum.status_message) - 1U] = '\0';
}

/* ---------------------------------------------------------------------
 * Calibration persistence
 * ------------------------------------------------------------------- */

static void olcum_load_calibration(void)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_app_data_path("olcum", OLCUM_CALIBRATION_FILE, path, sizeof(path)) != ESP_OK) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    olcum_calibration_t cal = {0};
    const size_t read = fread(&cal, 1, sizeof(cal), f);
    fclose(f);
    if (read != sizeof(cal) || cal.magic != OLCUM_CALIBRATION_MAGIC ||
        cal.version != OLCUM_CALIBRATION_VERSION) {
        return;
    }
    if (cal.px_per_mm_x > 0.5f && cal.px_per_mm_x < 50.0f) {
        olcum.px_per_mm_x = cal.px_per_mm_x;
    }
    if (cal.px_per_mm_y > 0.5f && cal.px_per_mm_y < 50.0f) {
        olcum.px_per_mm_y = cal.px_per_mm_y;
    }
}

static void olcum_save_calibration(void)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    const esp_err_t path_err = solar_os_storage_app_data_path("olcum", OLCUM_CALIBRATION_FILE, path, sizeof(path));
    if (path_err == SOLAR_OS_STORAGE_ERR_NO_SD_CARD) {
        olcum_set_status("No SD card - calibration not saved");
        return;
    }
    if (path_err != ESP_OK) {
        return;
    }
    const olcum_calibration_t cal = {
        .magic = OLCUM_CALIBRATION_MAGIC,
        .version = OLCUM_CALIBRATION_VERSION,
        .px_per_mm_x = olcum.px_per_mm_x,
        .px_per_mm_y = olcum.px_per_mm_y,
    };
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    (void)fwrite(&cal, 1, sizeof(cal), f);
    fflush(f);
    fclose(f);
}

/* ---------------------------------------------------------------------
 * Ruler View (Metric L-Ruler)
 * ------------------------------------------------------------------- */

static void olcum_draw_h_ruler(solar_os_gfx_t *gfx, int origin_x, int origin_y, int max_x, float px_per_mm)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, origin_x, origin_y, max_x, origin_y);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    const float max_mm = (float)(max_x - origin_x) / px_per_mm;
    for (int mm = 0; (float)mm <= max_mm; mm++) {
        const int x = origin_x + (int)((float)mm * px_per_mm);
        const int tick_len = (mm % 10 == 0) ? 16 : ((mm % 5 == 0) ? 10 : 5);
        solar_os_gfx_line(gfx, x, origin_y, x, origin_y + tick_len);
        if (mm % 10 == 0) {
            char label[8];
            snprintf(label, sizeof(label), "%d", mm / 10);
            solar_os_gfx_text(gfx, x + 2, origin_y + tick_len + 10, label);
        }
    }
}

static void olcum_draw_v_ruler(solar_os_gfx_t *gfx, int origin_x, int origin_y, int max_y, float px_per_mm)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, origin_x, origin_y, origin_x, max_y);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    const float max_mm = (float)(max_y - origin_y) / px_per_mm;
    for (int mm = 0; (float)mm <= max_mm; mm++) {
        const int y = origin_y + (int)((float)mm * px_per_mm);
        const int tick_len = (mm % 10 == 0) ? 16 : ((mm % 5 == 0) ? 10 : 5);
        solar_os_gfx_line(gfx, origin_x, y, origin_x + tick_len, y);
        if (mm % 10 == 0 && mm > 0) {
            char label[8];
            snprintf(label, sizeof(label), "%d", mm / 10);
            solar_os_gfx_text(gfx, origin_x + tick_len + 2, y + 4, label);
        }
    }
}

static void olcum_draw_ruler(solar_os_gfx_t *gfx, int width, int height)
{
    const int origin_x = 24;
    const int origin_y = OLCUM_HEADER_H + 24;

    olcum_draw_h_ruler(gfx, origin_x, origin_y, width - 8, olcum.px_per_mm_x);
    olcum_draw_v_ruler(gfx, origin_x, origin_y, height - OLCUM_FOOTER_H - 12, olcum.px_per_mm_y);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char info[96];
    const float screen_w_mm = (float)width / olcum.px_per_mm_x;
    const float screen_h_mm = (float)height / olcum.px_per_mm_y;
    snprintf(info, sizeof(info), "Screen Active Area: %.1f x %.1f mm (1:1 Physical Metric Scale)",
             (double)screen_w_mm, (double)screen_h_mm);
    solar_os_gfx_text(gfx, origin_x + 30, height - OLCUM_FOOTER_H - 10, info);
}

/* ---------------------------------------------------------------------
 * Metric Circles & Screw Gauges View
 * ------------------------------------------------------------------- */

static void olcum_draw_circles(solar_os_gfx_t *gfx, int width, int height)
{
    const float px_per_mm = (olcum.px_per_mm_x + olcum.px_per_mm_y) * 0.5f;

    /* 1. Left Section: Metric Screw Thread Diameters (M1.5 - M5) */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, 12, OLCUM_HEADER_H + 20, "BOLT & SCREW GAUGES");
    solar_os_gfx_line(gfx, 12, OLCUM_HEADER_H + 24, 175, OLCUM_HEADER_H + 24);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    int screw_y = OLCUM_HEADER_H + 45;
    for (size_t i = 0; i < OLCUM_SCREW_COUNT; i++) {
        const int cx = 35;
        const int cy = screw_y + (int)i * 32;
        const int radius_px = (int)(OLCUM_SCREWS[i].mm * px_per_mm * 0.5f + 0.5f);

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_circle(gfx, cx, cy, radius_px > 1 ? radius_px : 1);
        solar_os_gfx_fill_circle(gfx, cx, cy, 1);

        char label[32];
        snprintf(label, sizeof(label), "%s (%.1f mm)", OLCUM_SCREWS[i].label, (double)OLCUM_SCREWS[i].mm);
        solar_os_gfx_text(gfx, 60, cy + 4, label);
    }

    /* Vertical divider line */
    solar_os_gfx_line(gfx, 185, OLCUM_HEADER_H + 8, 185, height - OLCUM_FOOTER_H - 8);

    /* 2. Right Section: Concentric Circles (1cm, 2cm, 3cm, 4cm) */
    const int conc_cx = 295;
    const int conc_cy = OLCUM_HEADER_H + (height - OLCUM_HEADER_H - OLCUM_FOOTER_H) / 2;

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 198, OLCUM_HEADER_H + 20, "CONCENTRIC CIRCLES (1-4 cm)");
    solar_os_gfx_line(gfx, 198, OLCUM_HEADER_H + 24, 388, OLCUM_HEADER_H + 24);

    /* Crosshairs through concentric center */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, conc_cx - 100, conc_cy, conc_cx + 100, conc_cy);
    solar_os_gfx_line(gfx, conc_cx, conc_cy - 100, conc_cx, conc_cy + 100);
    solar_os_gfx_fill_circle(gfx, conc_cx, conc_cy, 2);

    /* Circles for 1cm (10mm), 2cm (20mm), 3cm (30mm), 4cm (40mm) */
    static const float conc_diameters_cm[] = {1.0f, 2.0f, 3.0f, 4.0f};
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    for (int i = 0; i < 4; i++) {
        float diam_mm = conc_diameters_cm[i] * 10.0f;
        int r_px = (int)(diam_mm * px_per_mm * 0.5f + 0.5f);
        solar_os_gfx_circle(gfx, conc_cx, conc_cy, r_px);

        char c_lbl[16];
        snprintf(c_lbl, sizeof(c_lbl), "Ø%dcm", i + 1);
        solar_os_gfx_text(gfx, conc_cx + r_px - 14, conc_cy - 4, c_lbl);
    }
}

/* ---------------------------------------------------------------------
 * Protractor & Angle Meter View
 * ------------------------------------------------------------------- */

static void olcum_draw_protractor(solar_os_gfx_t *gfx, int width, int height)
{
    const int cx = width / 2;
    const int body_h = height - OLCUM_HEADER_H - OLCUM_FOOTER_H;
    const int cy = OLCUM_HEADER_H + body_h / 2 + 8;
    int radius = (width < body_h ? width : body_h) / 2 - 20;
    if (radius < 25) radius = 25;

    /* 1. Protractor Circle */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_circle(gfx, cx, cy, radius);

    /* 2. Guide Crosshairs (+) inside the circle */
    solar_os_gfx_line(gfx, cx - radius, cy, cx + radius, cy);
    solar_os_gfx_line(gfx, cx, cy - radius, cx, cy + radius);
    solar_os_gfx_fill_circle(gfx, cx, cy, 3);

    /* 3. 360-Degree Graduation Ticks */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    for (int deg = 0; deg < 360; deg += 10) {
        const float rad = (float)deg * OLCUM_PI / 180.0f;
        const float s = sinf(rad);
        const float c = cosf(rad);
        const int outer_x = cx + (int)((float)radius * s);
        const int outer_y = cy - (int)((float)radius * c);
        const int inner_len = (deg % 30 == 0) ? 12 : ((deg % 10 == 0) ? 6 : 3);
        const int inner_x = cx + (int)((float)(radius - inner_len) * s);
        const int inner_y = cy - (int)((float)(radius - inner_len) * c);
        solar_os_gfx_line(gfx, inner_x, inner_y, outer_x, outer_y);

        if (deg % 30 == 0) {
            char label[8];
            snprintf(label, sizeof(label), "%d°", deg);
            const int lx = cx + (int)((float)(radius + 14) * s);
            const int ly = cy - (int)((float)(radius + 14) * c);
            solar_os_gfx_text(gfx, lx - 8, ly + 4, label);
        }
    }

    /* 4. Rotating Angle Arm */
    const float rad = olcum.angle_deg * OLCUM_PI / 180.0f;
    const int px = cx + (int)((float)radius * sinf(rad));
    const int py = cy - (int)((float)radius * cosf(rad));
    solar_os_gfx_line(gfx, cx, cy, px, py);
    solar_os_gfx_line(gfx, cx + 1, cy, px + 1, py); /* 2px thick arm */
    solar_os_gfx_fill_circle(gfx, px, py, 4);

    /* 5. Big Digital Angle Readout */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    char angle_label[32];
    snprintf(angle_label, sizeof(angle_label), "%.1f°", (double)olcum.angle_deg);
    const size_t tw = solar_os_gfx_text_width(gfx, angle_label);
    solar_os_gfx_text(gfx, cx - (int)tw / 2, OLCUM_HEADER_H + 24, angle_label);
}

/* ---------------------------------------------------------------------
 * Calibration View
 * ------------------------------------------------------------------- */

static void olcum_draw_calibration(solar_os_gfx_t *gfx, int width, int height)
{
    (void)width;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, 10, OLCUM_HEADER_H + 18,
                      "Compare against a real ruler. Left/Right: horizontal fine-tune,");
    solar_os_gfx_text(gfx, 10, OLCUM_HEADER_H + 32,
                      "Up/Down: vertical fine-tune. [Enter] saves to storage.");

    const int h_y = OLCUM_HEADER_H + 62;
    const int h_x0 = 20;
    const int h_len = (int)(50.0f * olcum.px_per_mm_x);
    solar_os_gfx_line(gfx, h_x0, h_y, h_x0 + h_len, h_y);
    solar_os_gfx_line(gfx, h_x0, h_y - 6, h_x0, h_y + 6);
    solar_os_gfx_line(gfx, h_x0 + h_len, h_y - 6, h_x0 + h_len, h_y + 6);
    solar_os_gfx_text(gfx, h_x0, h_y + 18, "This line should be exactly 50mm (horizontal)");

    const int v_x = 40;
    const int v_y0 = OLCUM_HEADER_H + 100;
    const int v_len = (int)(50.0f * olcum.px_per_mm_y);
    solar_os_gfx_line(gfx, v_x, v_y0, v_x, v_y0 + v_len);
    solar_os_gfx_line(gfx, v_x - 6, v_y0, v_x + 6, v_y0);
    solar_os_gfx_line(gfx, v_x - 6, v_y0 + v_len, v_x + 6, v_y0 + v_len);
    solar_os_gfx_text(gfx, v_x + 12, v_y0 + v_len / 2, "50mm (vertical)");

    char values[110];
    snprintf(values, sizeof(values), "px/mm: horizontal %.4f  vertical %.4f  (manufacturer default: %.4f / %.4f)",
              (double)olcum.px_per_mm_x, (double)olcum.px_per_mm_y,
              (double)OLCUM_PX_PER_MM_X, (double)OLCUM_PX_PER_MM_Y);
    solar_os_gfx_text(gfx, 10, height - OLCUM_FOOTER_H - 10, values);
}

/* ---------------------------------------------------------------------
 * Header and Footer
 * ------------------------------------------------------------------- */

static const char * const OLCUM_TAB_NAMES[OLCUM_VIEW_COUNT] = { "Ruler", "Circles", "Protractor", "Calibrate" };

static void olcum_build_header(solar_os_appbar_header_t *out)
{
    memset(out, 0, sizeof(*out));
    out->title = "Measure";
    out->show_back = true;
    out->tabs.names = OLCUM_TAB_NAMES;
    out->tabs.count = OLCUM_VIEW_COUNT;
    out->tabs.active_index = (size_t)olcum.view;
}

static void olcum_draw_header(solar_os_gfx_t *gfx)
{
    solar_os_appbar_header_t header;
    olcum_build_header(&header);
    solar_os_appbar_draw_header(gfx, &header);
}

/* Builds the current footer's shortcut chips into a caller-owned buffer,
 * returning the count. Same set used by both drawing and click hit-testing
 * so they can never disagree about what's on screen. */
static size_t olcum_build_footer_shortcuts(solar_os_appbar_shortcut_t *items, size_t max_items)
{
    size_t n = 0;
    if (olcum.view == OLCUM_VIEW_CALIBRATION && n < max_items) {
        items[n].key = '\n'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Save");
        n++;
    }
    return n;
}

static void olcum_draw_footer(solar_os_gfx_t *gfx, int width, int height)
{
    if (olcum.status_message[0] != '\0') {
        const int footer_h = solar_os_appbar_footer_height(gfx);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_rect(gfx, 0, height - footer_h, width, footer_h);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 8, height - footer_h / 4, olcum.status_message);
        olcum.status_message[0] = '\0'; /* one-shot: shown for exactly this render */
        return;
    }

    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = olcum_build_footer_shortcuts(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);
}

static void olcum_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    olcum_draw_header(gfx);

    switch (olcum.view) {
    case OLCUM_VIEW_CIRCLES:
        olcum_draw_circles(gfx, width, height);
        break;
    case OLCUM_VIEW_PROTRACTOR:
        olcum_draw_protractor(gfx, width, height);
        break;
    case OLCUM_VIEW_CALIBRATION:
        olcum_draw_calibration(gfx, width, height);
        break;
    case OLCUM_VIEW_RULER:
    default:
        olcum_draw_ruler(gfx, width, height);
        break;
    }

    olcum_draw_footer(gfx, width, height);
    solar_os_gfx_present(gfx);
    olcum.render_pending = false;
}

static esp_err_t olcum_start(solar_os_context_t *ctx)
{
    memset(&olcum, 0, sizeof(olcum));
    olcum.px_per_mm_x = OLCUM_PX_PER_MM_X;
    olcum.px_per_mm_y = OLCUM_PX_PER_MM_Y;
    olcum_load_calibration(); /* overrides the manufacturer default if a saved fine-tune exists */
    olcum.angle_deg = 45.0f;
    olcum.view = OLCUM_VIEW_RULER;
    olcum.render_pending = true;

    solar_os_context_set_graphics_active(ctx, true);
    olcum_render(ctx);
    return ESP_OK;
}

static void olcum_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
    olcum_save_calibration();
}

static bool olcum_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (ch == '\t' || ch == 'm' || ch == 'M' || ch == ' ') {
            olcum.view = (olcum.view + 1) % OLCUM_VIEW_COUNT;
            olcum_render(ctx);
            return true;
        }

        if (olcum.view == OLCUM_VIEW_PROTRACTOR) {
            if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A') {
                olcum.angle_deg -= 1.0f;
                if (olcum.angle_deg < 0.0f) olcum.angle_deg += 360.0f;
                olcum_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D') {
                olcum.angle_deg += 1.0f;
                if (olcum.angle_deg >= 360.0f) olcum.angle_deg -= 360.0f;
                olcum_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
                olcum.angle_deg -= 5.0f;
                if (olcum.angle_deg < 0.0f) olcum.angle_deg += 360.0f;
                olcum_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
                olcum.angle_deg += 5.0f;
                if (olcum.angle_deg >= 360.0f) olcum.angle_deg -= 360.0f;
                olcum_render(ctx);
                return true;
            }
        }

        if (olcum.view == OLCUM_VIEW_CALIBRATION) {
            if (ch == SOLAR_OS_KEY_LEFT) {
                olcum.px_per_mm_x *= 0.995f;
                olcum_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_RIGHT) {
                olcum.px_per_mm_x *= 1.005f;
                olcum_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_UP) {
                olcum.px_per_mm_y *= 1.005f;
                olcum_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_DOWN) {
                olcum.px_per_mm_y *= 0.995f;
                olcum_render(ctx);
                return true;
            }
            if (ch == '\n' || ch == '\r') {
                olcum_save_calibration();
                olcum_render(ctx);
                return true;
            }
        }

        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return false;

        solar_os_appbar_header_t header;
        olcum_build_header(&header);

        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, event->data.click.x, event->data.click.y, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                solar_os_context_request_exit(ctx);
            } else if (hit.kind == SOLAR_OS_APPBAR_HIT_TAB_ITEM && hit.index < OLCUM_VIEW_COUNT) {
                olcum.view = (olcum_view_t)hit.index;
                olcum_render(ctx);
            }
            return true;
        }

        solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
        const size_t count = olcum_build_footer_shortcuts(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };

        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, event->data.click.x, event->data.click.y, &fhit) &&
            fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM) {
            if (olcum.view == OLCUM_VIEW_CALIBRATION && items[fhit.index].key == '\n') {
                olcum_save_calibration();
                olcum_render(ctx);
            }
        }
        return true;
    }

    return false;
}

const solar_os_app_t solar_os_olcum_app = {
    .name = "olcum",
    .summary = "precision ruler, caliper and angle protractor",
    .flags = 0,
    .start = olcum_start,
    .stop = olcum_stop,
    .event = olcum_event,
    .state_slot = &olcum_state_ptr,
    .state_size = sizeof(olcum_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 100U,
    .worker_stack_bytes = 6144,
};
