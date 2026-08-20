#include "solar_os_photo_frame.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"
#include "solar_os_storage.h"
#include "solar_os_appbar.h"

#define PHOTO_MAX_FILES 64
#define PHOTO_MAX_PATH 128
#define PHOTO_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(PHOTO_STACK_SIZE);

/* Slideshow dwell times the Interval chip cycles through (seconds). */
static const uint32_t PHOTO_INTERVALS[] = { 30, 60, 180, 360 };
#define PHOTO_INTERVAL_COUNT (sizeof(PHOTO_INTERVALS) / sizeof(PHOTO_INTERVALS[0]))

typedef enum {
    PHOTO_VIEW_SLIDESHOW = 0,
    PHOTO_VIEW_LIBRARY,
    PHOTO_VIEW_COUNT
} photo_view_t;

static const char *const PHOTO_TAB_NAMES[PHOTO_VIEW_COUNT] = { "Slideshow", "Photos" };

typedef enum {
    PHOTO_MODE_ALL = 0,     /* every photo in the folder */
    PHOTO_MODE_SELECTED     /* only the ticked photos */
} photo_mode_t;

typedef struct {
    char filenames[PHOTO_MAX_FILES][PHOTO_MAX_PATH];
    char display_names[PHOTO_MAX_FILES][32];
    bool selected[PHOTO_MAX_FILES];
    size_t photo_count;
    size_t current_index;
    bool auto_play;
    uint32_t last_switch_ms;

    photo_view_t view;
    photo_mode_t mode;
    size_t interval_index;  /* into PHOTO_INTERVALS */
    size_t list_scroll;     /* first visible row in the library list */
} photo_frame_state_t;

static void *photo_frame_state_ptr;
#define pframe (*(photo_frame_state_t *)photo_frame_state_ptr)

static uint32_t pframe_interval_seconds(void)
{
    return PHOTO_INTERVALS[pframe.interval_index % PHOTO_INTERVAL_COUNT];
}

/* A photo is in the active playlist if we're showing all, or it's ticked. */
static bool pframe_is_active(size_t i)
{
    return pframe.mode == PHOTO_MODE_ALL || pframe.selected[i];
}

static size_t pframe_active_count(void)
{
    if (pframe.mode == PHOTO_MODE_ALL) return pframe.photo_count;
    size_t n = 0;
    for (size_t i = 0; i < pframe.photo_count; i++) {
        if (pframe.selected[i]) n++;
    }
    return n;
}

/* Returns the next active photo index from `cur` in direction dir (+1/-1),
 * wrapping; -1 if none are active. */
static int pframe_step(int cur, int dir)
{
    const int n = (int)pframe.photo_count;
    if (n == 0) return -1;
    for (int k = 0; k < n; k++) {
        cur = (cur + dir + n) % n;
        if (pframe_is_active((size_t)cur)) return cur;
    }
    return -1;
}

/* Makes sure current_index points at an active photo (used after a mode or
 * selection change). */
static void pframe_ensure_active(void)
{
    if (pframe.photo_count == 0) return;
    if (pframe.current_index < pframe.photo_count && pframe_is_active(pframe.current_index)) {
        return;
    }
    const int first = pframe_step((int)pframe.current_index, +1);
    pframe.current_index = first >= 0 ? (size_t)first : 0;
}

static void scan_photos_in_dir(const char *dir_path)
{
    if (dir_path == NULL) return;
    DIR *dir = opendir(dir_path);
    if (dir == NULL) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        const char *dot = strrchr(entry->d_name, '.');
        if (dot == NULL) continue;

        if (strcasecmp(dot, ".bmp") == 0 ||
            strcasecmp(dot, ".raw") == 0 ||
            strcasecmp(dot, ".xbm") == 0 ||
            strcasecmp(dot, ".pbm") == 0 ||
            strcasecmp(dot, ".bin") == 0 ||
            strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".png") == 0) {

            if (pframe.photo_count >= PHOTO_MAX_FILES) break;

            snprintf(pframe.filenames[pframe.photo_count], PHOTO_MAX_PATH, "%s/%s", dir_path, entry->d_name);
            strlcpy(pframe.display_names[pframe.photo_count], entry->d_name, 32);
            pframe.photo_count++;
        }
    }
    closedir(dir);
}

static void pframe_refresh_photos(void)
{
    pframe.photo_count = 0;
    if (solar_os_storage_sd_is_mounted()) {
        const char *sd_root = solar_os_storage_sd_mount_point();
        char path[64];
        snprintf(path, sizeof(path), "%s/photos", sd_root);
        scan_photos_in_dir(path);
    }
    if (solar_os_storage_flash_is_mounted()) {
        const char *flash_root = solar_os_storage_flash_mount_point();
        char path[64];
        snprintf(path, sizeof(path), "%s/photos", flash_root);
        scan_photos_in_dir(path);
    }
}

static void draw_bmp_image(solar_os_gfx_t *gfx, const char *filepath, int origin_x, int origin_y, int max_w, int max_h)
{
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) return;

    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f);
        return;
    }

    const uint32_t offset = *(uint32_t *)&hdr[10];
    const int32_t width = *(int32_t *)&hdr[18];
    const int32_t height = *(int32_t *)&hdr[22];
    const uint16_t bpp = *(uint16_t *)&hdr[28];
    const uint32_t compression = *(uint32_t *)&hdr[30];

    if (width <= 0 || height == 0 || compression != 0) {
        fclose(f);
        return;
    }

    const bool top_down = height < 0;
    const uint32_t abs_h = top_down ? (uint32_t)(-height) : (uint32_t)height;
    const uint32_t abs_w = (uint32_t)width;
    const uint32_t row_stride = ((abs_w * bpp + 31) / 32) * 4;

    uint8_t *row = (uint8_t *)malloc(row_stride);
    if (row == NULL) {
        fclose(f);
        return;
    }

    uint8_t pal[256];
    if (bpp <= 8) {
        uint32_t pal_entries = 1 << bpp;
        if (pal_entries > 256) pal_entries = 256;
        fseek(f, 54, SEEK_SET);
        for (uint32_t i = 0; i < pal_entries; i++) {
            uint8_t entry[4];
            if (fread(entry, 1, 4, f) == 4) {
                uint32_t gray = (entry[2] * 299 + entry[1] * 587 + entry[0] * 114) / 1000;
                pal[i] = (uint8_t)gray;
            } else {
                pal[i] = (i == 0) ? 255 : 0;
            }
        }
    }

    int draw_w = (int)abs_w;
    int draw_h = (int)abs_h;
    if (draw_w > max_w || draw_h > max_h) {
        float sx = (float)max_w / (float)draw_w;
        float sy = (float)max_h / (float)draw_h;
        float scale = sx < sy ? sx : sy;
        draw_w = (int)(abs_w * scale);
        draw_h = (int)(abs_h * scale);
    }
    const int start_x = origin_x + (max_w - draw_w) / 2;
    const int start_y = origin_y + (max_h - draw_h) / 2;

    for (uint32_t dy = 0; dy < (uint32_t)draw_h; dy++) {
        uint32_t sy = top_down ? (dy * abs_h / (uint32_t)draw_h) : (abs_h - 1 - (dy * abs_h / (uint32_t)draw_h));
        fseek(f, offset + sy * row_stride, SEEK_SET);
        if (fread(row, 1, row_stride, f) != row_stride) break;

        for (uint32_t dx = 0; dx < (uint32_t)draw_w; dx++) {
            uint32_t sx = dx * abs_w / (uint32_t)draw_w;
            bool is_dark = false;

            if (bpp == 1) {
                uint8_t bit = (row[sx / 8] >> (7 - (sx % 8))) & 1;
                is_dark = (pal[bit] < 128);
            } else if (bpp == 4) {
                uint8_t nibble = (sx % 2 == 0) ? (row[sx / 2] >> 4) : (row[sx / 2] & 0x0F);
                is_dark = (pal[nibble] < 128);
            } else if (bpp == 8) {
                is_dark = (pal[row[sx]] < 128);
            } else if (bpp == 24) {
                uint8_t b = row[sx * 3];
                uint8_t g = row[sx * 3 + 1];
                uint8_t r = row[sx * 3 + 2];
                is_dark = ((r * 299 + g * 587 + b * 114) / 1000 < 128);
            }

            if (is_dark) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_pixel(gfx, start_x + (int)dx, start_y + (int)dy);
            }
        }
    }

    free(row);
    fclose(f);
}

/* 1-based position of the current photo within the active playlist. */
static size_t pframe_active_ordinal(void)
{
    size_t ord = 0;
    for (size_t i = 0; i <= pframe.current_index && i < pframe.photo_count; i++) {
        if (pframe_is_active(i)) ord++;
    }
    return ord;
}

/* Builds the footer chips for the active view. Same set feeds draw + click. */
static size_t pframe_build_footer(solar_os_appbar_shortcut_t *items, size_t max_items)
{
    size_t n = 0;
    if (pframe.view == PHOTO_VIEW_SLIDESHOW) {
        if (n < max_items) { items[n].key = ' '; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), pframe.auto_play ? "Pause" : "Play"); n++; }
        if (n < max_items) { items[n].key = 'p'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Prev"); n++; }
        if (n < max_items) { items[n].key = 'n'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Next"); n++; }
        if (n < max_items) { items[n].key = 'i'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Int:%us", (unsigned)pframe_interval_seconds()); n++; }
    } else {
        if (n < max_items) { items[n].key = 'm'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Show:%s",
                     pframe.mode == PHOTO_MODE_ALL ? "All" : "Selected"); n++; }
        if (n < max_items) { items[n].key = 'r'; items[n].ctrl = false;
            snprintf(items[n].label, sizeof(items[n].label), "Rescan"); n++; }
    }
    return n;
}

/* Library list layout, shared by render and click hit-testing. */
#define PHOTO_LIST_ROW_H 20

static void pframe_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Shared header with view tabs; details ride in the status line. */
    solar_os_appbar_header_t header = {0};
    header.title = "Photo Frame";
    header.show_back = true;
    header.tabs.names = PHOTO_TAB_NAMES;
    header.tabs.count = PHOTO_VIEW_COUNT;
    header.tabs.active_index = (size_t)pframe.view;

    const size_t active = pframe_active_count();
    char status[80];
    if (pframe.view == PHOTO_VIEW_SLIDESHOW) {
        if (active > 0) {
            snprintf(status, sizeof(status), "%u/%u  %s  %us",
                     (unsigned)pframe_active_ordinal(), (unsigned)active,
                     pframe.auto_play ? "Auto" : "Paused",
                     (unsigned)pframe_interval_seconds());
        } else {
            snprintf(status, sizeof(status), "No photos to show");
        }
    } else {
        snprintf(status, sizeof(status), "%u photos, %u selected",
                 (unsigned)pframe.photo_count, (unsigned)active);
    }
    header.status_line = status;
    solar_os_appbar_draw_header(gfx, &header);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    const int hh = solar_os_appbar_header_height(gfx);
    const int sh = solar_os_appbar_status_line_height(gfx);
    const int fh = solar_os_appbar_footer_height(gfx);
    const int body_top = hh + sh + 2;
    const int body_h = screen_h - body_top - fh - 2;

    if (pframe.view == PHOTO_VIEW_SLIDESHOW) {
        const int frame_x = 8;
        const int frame_y = body_top;
        const int frame_w = screen_w - 16;
        const int frame_h = body_h;

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, frame_x, frame_y, frame_w, frame_h);

        if (active == 0) {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, frame_x + 24, frame_y + 40,
                              pframe.photo_count == 0 ? "No photos found" : "Nothing selected");
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            if (pframe.photo_count == 0) {
                solar_os_gfx_text(gfx, frame_x + 24, frame_y + 68, "Put .bmp/.jpg/.png files in:");
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
                solar_os_gfx_text(gfx, frame_x + 24, frame_y + 88, "/sdcard/photos/");
            } else {
                solar_os_gfx_text(gfx, frame_x + 24, frame_y + 68,
                                  "Open the Photos tab to tick some,");
                solar_os_gfx_text(gfx, frame_x + 24, frame_y + 84,
                                  "or switch Show to All.");
            }
        } else {
            const char *filepath = pframe.filenames[pframe.current_index];
            const char *dot = strrchr(filepath, '.');
            if (dot != NULL && strcasecmp(dot, ".bmp") == 0) {
                draw_bmp_image(gfx, filepath, frame_x + 2, frame_y + 2, frame_w - 4, frame_h - 4);
            } else {
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
                solar_os_gfx_text(gfx, frame_x + 10, frame_y + 22,
                                  pframe.display_names[pframe.current_index]);
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                solar_os_gfx_text(gfx, frame_x + 10, frame_y + 42,
                                  "(preview only for .bmp files)");
            }
        }
    } else {
        /* Library list with tick boxes. */
        const int list_x = 8;
        const int list_y = body_top;
        const int list_w = screen_w - 16;
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, list_x, list_y, list_w, body_h);

        if (pframe.photo_count == 0) {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, list_x + 12, list_y + 24,
                              "No photos in /sdcard/photos/. Tap Rescan after adding files.");
        } else {
            const int rows = body_h / PHOTO_LIST_ROW_H;
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            for (int r = 0; r < rows; r++) {
                const size_t idx = pframe.list_scroll + (size_t)r;
                if (idx >= pframe.photo_count) break;
                const int ry = list_y + r * PHOTO_LIST_ROW_H;
                const bool is_cur = (idx == pframe.current_index);
                if (is_cur) {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_fill_rect(gfx, list_x + 1, ry, list_w - 2, PHOTO_LIST_ROW_H);
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
                } else {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                }
                /* Tick box + name. */
                const char *box = pframe.selected[idx] ? "[x]" : "[ ]";
                solar_os_gfx_text(gfx, list_x + 8, ry + 14, box);
                solar_os_gfx_text(gfx, list_x + 40, ry + 14, pframe.display_names[idx]);
            }
        }
    }

    /* 2. Shared footer chips. */
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = pframe_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);

    solar_os_gfx_present(gfx);
}

static esp_err_t pframe_start(solar_os_context_t *ctx)
{
    pframe.auto_play = true;
    pframe.current_index = 0;
    pframe.last_switch_ms = (uint32_t)(esp_timer_get_time() / 1000U);
    pframe.view = PHOTO_VIEW_SLIDESHOW;
    pframe.mode = PHOTO_MODE_ALL;
    pframe.interval_index = 0; /* 30s */
    pframe.list_scroll = 0;
    memset(pframe.selected, 0, sizeof(pframe.selected));

    pframe_refresh_photos();
    pframe_ensure_active();
    solar_os_context_set_graphics_active(ctx, true);
    pframe_render(ctx);
    return ESP_OK;
}

static void pframe_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

/* Advances the slideshow by one active photo (dir +1/-1) and resets the timer. */
static void pframe_advance(solar_os_context_t *ctx, int dir)
{
    const int nx = pframe_step((int)pframe.current_index, dir);
    if (nx >= 0) {
        pframe.current_index = (size_t)nx;
        pframe.last_switch_ms = (uint32_t)(esp_timer_get_time() / 1000U);
        pframe_render(ctx);
    }
}

static void pframe_toggle_play(solar_os_context_t *ctx)
{
    pframe.auto_play = !pframe.auto_play;
    pframe.last_switch_ms = (uint32_t)(esp_timer_get_time() / 1000U);
    pframe_render(ctx);
}

static void pframe_cycle_interval(solar_os_context_t *ctx)
{
    pframe.interval_index = (pframe.interval_index + 1) % PHOTO_INTERVAL_COUNT;
    pframe.last_switch_ms = (uint32_t)(esp_timer_get_time() / 1000U);
    pframe_render(ctx);
}

static void pframe_toggle_mode(solar_os_context_t *ctx)
{
    pframe.mode = pframe.mode == PHOTO_MODE_ALL ? PHOTO_MODE_SELECTED : PHOTO_MODE_ALL;
    pframe_ensure_active();
    pframe_render(ctx);
}

static void pframe_dispatch_footer_key(solar_os_context_t *ctx, char key)
{
    switch (key) {
    case ' ': pframe_toggle_play(ctx); break;
    case 'p': pframe_advance(ctx, -1); break;
    case 'n': pframe_advance(ctx, +1); break;
    case 'i': pframe_cycle_interval(ctx); break;
    case 'm': pframe_toggle_mode(ctx); break;
    case 'r':
        pframe_refresh_photos();
        memset(pframe.selected, 0, sizeof(pframe.selected));
        pframe.current_index = 0;
        pframe.list_scroll = 0;
        pframe_ensure_active();
        pframe_render(ctx);
        break;
    default: break;
    }
}

static bool pframe_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
        if (pframe.view == PHOTO_VIEW_SLIDESHOW && pframe.auto_play &&
            pframe_active_count() > 1) {
            if (now - pframe.last_switch_ms >= (pframe_interval_seconds() * 1000U)) {
                pframe.last_switch_ms = now;
                pframe_advance(ctx, +1);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        solar_os_appbar_header_t header = {0};
        header.show_back = true;
        header.tabs.names = PHOTO_TAB_NAMES;
        header.tabs.count = PHOTO_VIEW_COUNT;
        header.tabs.active_index = (size_t)pframe.view;
        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, px, py, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                solar_os_context_request_exit(ctx);
            } else if (hit.kind == SOLAR_OS_APPBAR_HIT_TAB_ITEM && hit.index < PHOTO_VIEW_COUNT) {
                pframe.view = (photo_view_t)hit.index;
                pframe_render(ctx);
            }
            return true;
        }

        solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
        const size_t count = pframe_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < count) {
                pframe_dispatch_footer_key(ctx, items[fhit.index].key);
            }
            return true;
        }

        const int hh = solar_os_appbar_header_height(gfx);
        const int sh = solar_os_appbar_status_line_height(gfx);
        const int body_top = hh + sh + 2;

        if (pframe.view == PHOTO_VIEW_SLIDESHOW) {
            /* Tap left third = prev, right third = next, middle = play/pause. */
            const int w = (int)solar_os_gfx_width(gfx);
            if (py >= body_top) {
                if (px < w / 3) pframe_advance(ctx, -1);
                else if (px > (2 * w) / 3) pframe_advance(ctx, +1);
                else pframe_toggle_play(ctx);
            }
            return true;
        }

        /* Library: tap a row to toggle its tick. */
        if (pframe.photo_count > 0 && py >= body_top) {
            const int r = (py - body_top) / PHOTO_LIST_ROW_H;
            const size_t idx = pframe.list_scroll + (size_t)r;
            if (r >= 0 && idx < pframe.photo_count) {
                pframe.selected[idx] = !pframe.selected[idx];
                pframe.current_index = idx;
                pframe_render(ctx);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_SCROLL) {
        if (pframe.view == PHOTO_VIEW_LIBRARY && pframe.photo_count > 0) {
            const bool down = event->data.scroll.delta < 0;
            if (down && pframe.list_scroll + 1 < pframe.photo_count) {
                pframe.list_scroll++;
            } else if (!down && pframe.list_scroll > 0) {
                pframe.list_scroll--;
            }
            pframe_render(ctx);
        } else if (pframe.view == PHOTO_VIEW_SLIDESHOW) {
            pframe_advance(ctx, event->data.scroll.delta < 0 ? +1 : -1);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;
        if (ch == ' ') { pframe_toggle_play(ctx); return true; }
        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D' || ch == 'l' || ch == 'L' || ch == 'n' || ch == 'N') {
            pframe_advance(ctx, +1);
            return true;
        }
        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A' || ch == 'h' || ch == 'H' || ch == 'p' || ch == 'P') {
            pframe_advance(ctx, -1);
            return true;
        }
        if (ch == SOLAR_OS_KEY_UP) {
            if (pframe.view == PHOTO_VIEW_LIBRARY && pframe.list_scroll > 0) { pframe.list_scroll--; pframe_render(ctx); }
            return true;
        }
        if (ch == SOLAR_OS_KEY_DOWN) {
            if (pframe.view == PHOTO_VIEW_LIBRARY && pframe.list_scroll + 1 < pframe.photo_count) { pframe.list_scroll++; pframe_render(ctx); }
            return true;
        }
        if (ch == '\t') {
            pframe.view = (pframe.view == PHOTO_VIEW_SLIDESHOW) ? PHOTO_VIEW_LIBRARY : PHOTO_VIEW_SLIDESHOW;
            pframe_render(ctx);
            return true;
        }
        if (ch == '\r' || ch == '\n') {
            /* In the library, Enter ticks the current photo. */
            if (pframe.view == PHOTO_VIEW_LIBRARY && pframe.current_index < pframe.photo_count) {
                pframe.selected[pframe.current_index] = !pframe.selected[pframe.current_index];
                pframe_render(ctx);
            }
            return true;
        }
        if (ch == 'i' || ch == 'I') { pframe_cycle_interval(ctx); return true; }
        if (ch == 'm' || ch == 'M') { pframe_toggle_mode(ctx); return true; }
        if (ch == 'r' || ch == 'R') { pframe_dispatch_footer_key(ctx, 'r'); return true; }
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_photo_frame_app = {
    .name = "photos",
    .summary = "photo frame and slideshow viewer",
    .flags = 0,
    .start = pframe_start,
    .stop = pframe_stop,
    .event = pframe_event,
    .state_slot = &photo_frame_state_ptr,
    .state_size = sizeof(photo_frame_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 1000U,
    .worker_stack_bytes = PHOTO_STACK_SIZE,
};
