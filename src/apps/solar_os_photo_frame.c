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

#define PHOTO_MAX_FILES 64
#define PHOTO_MAX_PATH 128
#define PHOTO_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(PHOTO_STACK_SIZE);

typedef struct {
    char filenames[PHOTO_MAX_FILES][PHOTO_MAX_PATH];
    char display_names[PHOTO_MAX_FILES][32];
    size_t photo_count;
    size_t current_index;
    bool auto_play;
    uint32_t interval_seconds;
    uint32_t last_switch_ms;
} photo_frame_state_t;

static void *photo_frame_state_ptr;
#define pframe (*(photo_frame_state_t *)photo_frame_state_ptr)

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
        snprintf(path, sizeof(path), "%s/screencapture", sd_root);
        scan_photos_in_dir(path);
        snprintf(path, sizeof(path), "%s/screenshots", sd_root);
        scan_photos_in_dir(path);
        snprintf(path, sizeof(path), "%s/screenshot", sd_root);
        scan_photos_in_dir(path);
        snprintf(path, sizeof(path), "%s/photos", sd_root);
        scan_photos_in_dir(path);
        snprintf(path, sizeof(path), "%s/images", sd_root);
        scan_photos_in_dir(path);
        scan_photos_in_dir(sd_root);
    }
    if (solar_os_storage_flash_is_mounted()) {
        const char *flash_root = solar_os_storage_flash_mount_point();
        char path[64];
        snprintf(path, sizeof(path), "%s/screencapture", flash_root);
        scan_photos_in_dir(path);
        snprintf(path, sizeof(path), "%s/photos", flash_root);
        scan_photos_in_dir(path);
        snprintf(path, sizeof(path), "%s/images", flash_root);
        scan_photos_in_dir(path);
        scan_photos_in_dir(flash_root);
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

static void pframe_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header Bar */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 16, "PHOTO GALLERY");

    char top_info[64];
    if (pframe.photo_count > 0) {
        snprintf(top_info, sizeof(top_info), "[%u/%u] %s (%us)",
                 (unsigned)(pframe.current_index + 1),
                 (unsigned)pframe.photo_count,
                 pframe.auto_play ? "AUTO" : "PAUSED",
                 (unsigned)pframe.interval_seconds);
    } else {
        snprintf(top_info, sizeof(top_info), "No images found");
    }
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t tw = solar_os_gfx_text_width(gfx, top_info);
    solar_os_gfx_text(gfx, screen_w - (int)tw - 8, 16, top_info);

    /* 2. Main Photo Canvas (X: 10..390, Y: 28..274) */
    const int frame_x = 8;
    const int frame_y = 26;
    const int frame_w = screen_w - 16;
    const int frame_h = 248;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, frame_x, frame_y, frame_w, frame_h);

    if (pframe.photo_count == 0) {
        /* Placeholder / Instructions */
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 40, 80, "SolarOS Photo & Screenshot Gallery");

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 40, 110, "Auto-scans the following folders on SD card:");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 40, 130, "/sdcard/screencapture/ (PrtSc screenshots)");
        solar_os_gfx_text(gfx, 40, 150, "/sdcard/photos/   and   /sdcard/images/");

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 40, 190, "Tip: Press PrtSc / F12 to take a screenshot anytime.");
        solar_os_gfx_text(gfx, 40, 210, "Press 'R' to rescan files.");
    } else {
        const char *filepath = pframe.filenames[pframe.current_index];
        const char *dot = strrchr(filepath, '.');
        if (dot != NULL && strcasecmp(dot, ".bmp") == 0) {
            draw_bmp_image(gfx, filepath, frame_x + 2, frame_y + 2, frame_w - 4, frame_h - 4);
        } else {
            const char *name = pframe.display_names[pframe.current_index];
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, frame_x + 10, frame_y + 20, name);
        }
    }

    /* 3. Footer Bar */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 293, "[SPACE] Play/Pause | [LEFT/RIGHT] Next/Prev | [R] Rescan | [ESC] Exit");

    solar_os_gfx_present(gfx);
}

static esp_err_t pframe_start(solar_os_context_t *ctx)
{
    pframe.auto_play = true;
    pframe.interval_seconds = 10;
    pframe.current_index = 0;
    pframe.last_switch_ms = (uint32_t)(esp_timer_get_time() / 1000U);

    pframe_refresh_photos();
    solar_os_context_set_graphics_active(ctx, true);
    pframe_render(ctx);
    return ESP_OK;
}

static void pframe_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool pframe_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
        if (pframe.auto_play && pframe.photo_count > 1) {
            if (now - pframe.last_switch_ms >= (pframe.interval_seconds * 1000U)) {
                pframe.last_switch_ms = now;
                pframe.current_index = (pframe.current_index + 1) % pframe.photo_count;
                pframe_render(ctx);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;
        if (ch == ' ') {
            pframe.auto_play = !pframe.auto_play;
            pframe.last_switch_ms = (uint32_t)(esp_timer_get_time() / 1000U);
            pframe_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D' || ch == 'l' || ch == 'L') {
            if (pframe.photo_count > 0) {
                pframe.current_index = (pframe.current_index + 1) % pframe.photo_count;
                pframe.last_switch_ms = (uint32_t)(esp_timer_get_time() / 1000U);
                pframe_render(ctx);
            }
            return true;
        }
        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A' || ch == 'h' || ch == 'H') {
            if (pframe.photo_count > 0) {
                if (pframe.current_index > 0) {
                    pframe.current_index--;
                } else {
                    pframe.current_index = pframe.photo_count - 1;
                }
                pframe.last_switch_ms = (uint32_t)(esp_timer_get_time() / 1000U);
                pframe_render(ctx);
            }
            return true;
        }
        if (ch == 'r' || ch == 'R') {
            pframe_refresh_photos();
            pframe.current_index = 0;
            pframe_render(ctx);
            return true;
        }
        if (ch == '1') pframe.interval_seconds = 5;
        if (ch == '2') pframe.interval_seconds = 10;
        if (ch == '3') pframe.interval_seconds = 30;
        if (ch == '4') pframe.interval_seconds = 60;
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
    .worker_stack_bytes = PHOTO_STACK_SIZE,
};
