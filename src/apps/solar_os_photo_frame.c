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
        snprintf(path, sizeof(path), "%s/photos", sd_root);
        scan_photos_in_dir(path);
        snprintf(path, sizeof(path), "%s/images", sd_root);
        scan_photos_in_dir(path);
        scan_photos_in_dir(sd_root);
    }
    if (solar_os_storage_flash_is_mounted()) {
        const char *flash_root = solar_os_storage_flash_mount_point();
        char path[64];
        snprintf(path, sizeof(path), "%s/photos", flash_root);
        scan_photos_in_dir(path);
        scan_photos_in_dir(flash_root);
    }
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
    solar_os_gfx_text(gfx, 8, 16, "PHOTO FRAME");

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

    /* 2. Main Photo Canvas (X: 10..390, Y: 30..270) */
    const int frame_x = 12;
    const int frame_y = 30;
    const int frame_w = screen_w - 24;
    const int frame_h = 240;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, frame_x, frame_y, frame_w, frame_h);
    solar_os_gfx_rect(gfx, frame_x + 2, frame_y + 2, frame_w - 4, frame_h - 4);

    if (pframe.photo_count == 0) {
        /* Placeholder / Instructions */
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 40, 90, "SD Card Photo Slideshow");

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 40, 120, "Place your .bmp, .raw or 1-bit images inside:");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 40, 140, "/sdcard/photos/   or   /sdcard/images/");

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 40, 175, "Press 'R' to rescan files at any time.");

        /* Draw a demo landscape silhouette */
        solar_os_gfx_circle(gfx, 280, 120, 24); /* Sun */
        solar_os_gfx_line(gfx, 60, 240, 160, 160);
        solar_os_gfx_line(gfx, 160, 160, 240, 240);
        solar_os_gfx_line(gfx, 200, 240, 280, 180);
        solar_os_gfx_line(gfx, 280, 180, 340, 240);
    } else {
        /* Render image name */
        const char *name = pframe.display_names[pframe.current_index];
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, frame_x + 10, frame_y + 20, name);

        /* Draw decorative frame center artwork */
        solar_os_gfx_rect(gfx, frame_x + 20, frame_y + 35, frame_w - 40, frame_h - 50);
        solar_os_gfx_circle(gfx, frame_x + frame_w / 2, frame_y + frame_h / 2, 45);
        solar_os_gfx_fill_circle(gfx, frame_x + frame_w / 2, frame_y + frame_h / 2, 25);
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
