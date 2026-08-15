#include "solar_os_video_player.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_resource_limits.h"
#include "solar_os_stb_image.h"
#include "solar_os_storage.h"
#include "solar_os_vector.h"

#define VIDEO_STACK_SIZE 16384
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(VIDEO_STACK_SIZE);

#define VIDEO_MAX_FILES 64
#define VIDEO_MAX_CANVAS_PIXELS (512U * 512U)
#define VIDEO_MAX_STORED_PIXELS (4U * 1024U * 1024U)

static const char *TAG = "solar_os_video_player";

typedef struct {
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char name[48];
    bool is_gif;
} video_file_entry_t;

typedef struct {
    uint8_t *gray;
    uint32_t width;
    uint32_t height;
    uint32_t frame_count;
    uint32_t frame_index;
    uint32_t *frame_delays_ms;
    int64_t next_frame_us;
    float speed;
    bool playing;
    bool loop;
    bool show_osd;
    uint32_t osd_until_ms;
    bool in_picker;
    char current_path[SOLAR_OS_STORAGE_PATH_MAX];
    char status_msg[64];
    video_file_entry_t files[VIDEO_MAX_FILES];
    size_t file_count;
    size_t selected_file;
} video_player_state_t;

static void *video_state_ptr;
#define vplay (*(video_player_state_t *)video_state_ptr)

static solar_os_gfx_color_t video_gray_to_color(uint8_t gray)
{
    if (gray < 48) {
        return SOLAR_OS_GFX_COLOR_BLACK;
    } else if (gray < 112) {
        return SOLAR_OS_GFX_COLOR_DARK;
    } else if (gray < 192) {
        return SOLAR_OS_GFX_COLOR_LIGHT;
    }
    return SOLAR_OS_GFX_COLOR_WHITE;
}

static void video_free_media(void)
{
    if (vplay.gray != NULL) {
        solar_os_memory_free(vplay.gray);
        vplay.gray = NULL;
    }
    if (vplay.frame_delays_ms != NULL) {
        solar_os_memory_free(vplay.frame_delays_ms);
        vplay.frame_delays_ms = NULL;
    }
    vplay.width = 0;
    vplay.height = 0;
    vplay.frame_count = 0;
    vplay.frame_index = 0;
}

static esp_err_t video_load_gif(const char *path)
{
    if (path == NULL || path[0] == '\0') return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Cannot open file");
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 6 * 1024 * 1024) {
        fclose(f);
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "File too large");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *raw_buf = solar_os_memory_alloc((size_t)fsize, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "video.raw");
    if (raw_buf == NULL) {
        fclose(f);
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    if (fread(raw_buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        solar_os_memory_free(raw_buf);
        fclose(f);
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Read error");
        return ESP_FAIL;
    }
    fclose(f);

    solar_os_stb_gif_animation_t anim = {0};
    esp_err_t err = solar_os_stb_decode_gif_gray(raw_buf,
                                                (size_t)fsize,
                                                VIDEO_MAX_CANVAS_PIXELS,
                                                VIDEO_MAX_STORED_PIXELS,
                                                400,
                                                300,
                                                solar_os_vector_rgba_to_gray_scaled,
                                                &anim);
    solar_os_memory_free(raw_buf);

    if (err == ESP_OK && anim.gray != NULL && anim.frame_count > 0) {
        video_free_media();
        vplay.width = anim.width;
        vplay.height = anim.height;
        vplay.frame_count = anim.frame_count;
        vplay.gray = anim.gray;
        vplay.frame_delays_ms = anim.delays_ms;
        vplay.frame_index = 0;
        vplay.playing = true;
        vplay.loop = true;
        vplay.speed = 1.0f;
        vplay.next_frame_us = esp_timer_get_time();
        vplay.in_picker = false;
        vplay.show_osd = true;
        vplay.osd_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 3000;
        strlcpy(vplay.current_path, path, sizeof(vplay.current_path));
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Loaded %" PRIu32 " frames (%\" PRIu32 \"x%\" PRIu32 \")",
                 vplay.frame_count, vplay.width, vplay.height);
        return ESP_OK;
    }

    solar_os_stb_gif_animation_free(&anim);
    snprintf(vplay.status_msg, sizeof(vplay.status_msg), "GIF decode failed");
    return err;
}

static void video_scan_folder(const char *dir_path)
{
    if (dir_path == NULL) return;
    DIR *dir = opendir(dir_path);
    if (dir == NULL) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        const char *dot = strrchr(entry->d_name, '.');
        if (dot == NULL) continue;

        if (strcasecmp(dot, ".gif") == 0 ||
            strcasecmp(dot, ".mjpeg") == 0 ||
            strcasecmp(dot, ".bmp") == 0 ||
            strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".png") == 0) {
            if (vplay.file_count < VIDEO_MAX_FILES) {
                video_file_entry_t *e = &vplay.files[vplay.file_count];
                snprintf(e->path, sizeof(e->path), "%s/%s", dir_path, entry->d_name);
                strlcpy(e->name, entry->d_name, sizeof(e->name));
                e->is_gif = (strcasecmp(dot, ".gif") == 0);
                vplay.file_count++;
            }
        }
    }
    closedir(dir);
}

static void video_refresh_file_list(void)
{
    vplay.file_count = 0;
    vplay.selected_file = 0;

    video_scan_folder("/sdcard/gifs");
    video_scan_folder("/sdcard/videos");
    video_scan_folder("/sdcard/photos");
    video_scan_folder("/sdcard");
    video_scan_folder("/flash/gifs");
    video_scan_folder("/flash/videos");
}

static void video_draw_frame(solar_os_gfx_t *gfx, int screen_w, int screen_h)
{
    if (vplay.gray == NULL || vplay.width == 0 || vplay.height == 0) return;

    const size_t frame_bytes = (size_t)vplay.width * vplay.height;
    const uint8_t *cur_gray = vplay.gray + ((size_t)vplay.frame_index * frame_bytes);

    /* Center or scale to fit */
    int dw = (int)vplay.width;
    int dh = (int)vplay.height;

    float aspect = (float)dw / (float)dh;
    if (dw > screen_w || dh > screen_h || (dw < screen_w && dh < screen_h)) {
        dw = screen_w;
        dh = (int)((float)screen_w / aspect);
        if (dh > screen_h) {
            dh = screen_h;
            dw = (int)((float)screen_h * aspect);
        }
    }

    const int ox = (screen_w - dw) / 2;
    const int oy = (screen_h - dh) / 2;

    for (int dy = 0; dy < dh; dy++) {
        const int target_y = oy + dy;
        if (target_y < 0 || target_y >= screen_h) continue;

        const uint32_t sy = (uint32_t)(((uint64_t)dy * vplay.height) / (uint32_t)dh);
        solar_os_gfx_color_t run_color = SOLAR_OS_GFX_COLOR_WHITE;
        int run_start = ox;
        bool run_active = false;

        for (int dx = 0; dx < dw; dx++) {
            const int target_x = ox + dx;
            if (target_x < 0 || target_x >= screen_w) continue;

            const uint32_t sx = (uint32_t)(((uint64_t)dx * vplay.width) / (uint32_t)dw);
            const uint8_t sample = cur_gray[(size_t)sy * vplay.width + sx];
            const solar_os_gfx_color_t color = video_gray_to_color(sample);

            if (!run_active) {
                run_active = true;
                run_color = color;
                run_start = target_x;
            } else if (color != run_color) {
                solar_os_gfx_set_color(gfx, run_color);
                solar_os_gfx_fill_rect(gfx, run_start, target_y, target_x - run_start, 1);
                run_color = color;
                run_start = target_x;
            }
        }

        if (run_active) {
            solar_os_gfx_set_color(gfx, run_color);
            solar_os_gfx_fill_rect(gfx, run_start, target_y, (ox + dw) - run_start, 1);
        }
    }
}

static void video_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    if (vplay.in_picker) {
        /* File Picker View */
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 24);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 8, 16, "CINEMA / VIDEO & GIF PLAYER");

        char count_txt[32];
        snprintf(count_txt, sizeof(count_txt), "%u media files", (unsigned)vplay.file_count);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        const size_t cw = solar_os_gfx_text_width(gfx, count_txt);
        solar_os_gfx_text(gfx, screen_w - (int)cw - 8, 16, count_txt);

        if (vplay.file_count == 0) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, 40, 100, "No GIF or Video files found on SD card.");
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 40, 130, "Put .gif or .mjpeg files into /sdcard/gifs or /sdcard");
            solar_os_gfx_text(gfx, 40, 155, "You can use the File Server app to upload them via Wi-Fi!");
        } else {
            const size_t page_size = 7;
            const size_t page = vplay.selected_file / page_size;
            const size_t start_idx = page * page_size;

            for (size_t i = 0; i < page_size && (start_idx + i) < vplay.file_count; i++) {
                const size_t idx = start_idx + i;
                const int y = 38 + (int)i * 32;
                const bool is_sel = (idx == vplay.selected_file);

                if (is_sel) {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_fill_rect(gfx, 10, y, screen_w - 20, 28);
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
                } else {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_rect(gfx, 10, y, screen_w - 20, 28);
                }

                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
                solar_os_gfx_text(gfx, 24, y + 19, vplay.files[idx].name);

                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                solar_os_gfx_text(gfx, screen_w - 110, y + 19, vplay.files[idx].is_gif ? "[ANIMATED GIF]" : "[IMAGE/VIDEO]");
            }
        }

        /* Footer */
        solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 8, 293, "Select: ARROWS | Play: ENTER | Refresh: R | Exit: ESC");
        solar_os_gfx_present(gfx);
        return;
    }

    /* Video Playback Mode */
    video_draw_frame(gfx, screen_w, screen_h);

    /* OSD HUD Overlay */
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (vplay.show_osd || now_ms < vplay.osd_until_ms || !vplay.playing) {
        /* Top Bar */
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 22);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

        const char *slash = strrchr(vplay.current_path, '/');
        const char *fname = slash != NULL ? slash + 1 : vplay.current_path;
        solar_os_gfx_text(gfx, 8, 15, fname);

        char meta_txt[64];
        snprintf(meta_txt, sizeof(meta_txt), "%" PRIu32 "x%" PRIu32 " | %.1fx | %s",
                 vplay.width, vplay.height, vplay.speed, vplay.loop ? "LOOP" : "ONCE");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        const size_t mw = solar_os_gfx_text_width(gfx, meta_txt);
        solar_os_gfx_text(gfx, screen_w - (int)mw - 8, 15, meta_txt);

        /* Bottom Timeline & Controls */
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_rect(gfx, 0, screen_h - 26, screen_w, 26);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);

        /* Progress Bar */
        solar_os_gfx_rect(gfx, 10, screen_h - 20, screen_w - 20, 6);
        if (vplay.frame_count > 1) {
            const int fill_w = (int)(((uint64_t)vplay.frame_index * (screen_w - 20)) / (vplay.frame_count - 1));
            solar_os_gfx_fill_rect(gfx, 10, screen_h - 20, fill_w, 6);
        }

        char ctrl_txt[80];
        snprintf(ctrl_txt, sizeof(ctrl_txt), "[%s] Frame %" PRIu32 "/%" PRIu32 " | [SPACE] Play/Pause | [ARROWS] Step/Speed | [ESC] Picker",
                 vplay.playing ? "PLAY" : "PAUSE", vplay.frame_index + 1, vplay.frame_count);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 8, screen_h - 4, ctrl_txt);
    }

    solar_os_gfx_present(gfx);
}

static esp_err_t video_start(solar_os_context_t *ctx)
{
    memset(&vplay, 0, sizeof(vplay));
    vplay.speed = 1.0f;
    vplay.loop = true;
    vplay.playing = true;

    solar_os_context_set_graphics_active(ctx, true);

    const int argc = solar_os_context_argc(ctx);
    if (argc > 1) {
        const char *launch_path = solar_os_context_argv(ctx, 1);
        if (launch_path != NULL && launch_path[0] != '\0') {
            esp_err_t err = video_load_gif(launch_path);
            if (err == ESP_OK) {
                video_render(ctx);
                return ESP_OK;
            }
        }
    }

    vplay.in_picker = true;
    video_refresh_file_list();
    video_render(ctx);
    return ESP_OK;
}

static void video_stop(solar_os_context_t *ctx)
{
    video_free_media();
    solar_os_context_set_graphics_active(ctx, false);
}

static bool video_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    const int64_t now_us = esp_timer_get_time();

    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (!vplay.in_picker && vplay.playing && vplay.frame_count > 1 && vplay.gray != NULL) {
            if (now_us >= vplay.next_frame_us) {
                vplay.frame_index = (vplay.frame_index + 1) % vplay.frame_count;

                uint32_t delay_ms = 100;
                if (vplay.frame_delays_ms != NULL) {
                    delay_ms = vplay.frame_delays_ms[vplay.frame_index];
                    if (delay_ms == 0) delay_ms = 100;
                }
                delay_ms = (uint32_t)((float)delay_ms / vplay.speed);
                if (delay_ms < 10) delay_ms = 10;

                vplay.next_frame_us = now_us + (int64_t)delay_ms * 1000LL;
                video_render(ctx);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (vplay.in_picker) {
            if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
                if (vplay.selected_file > 0) vplay.selected_file--;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
                if (vplay.selected_file + 1 < vplay.file_count) vplay.selected_file++;
                video_render(ctx);
                return true;
            }
            if (ch == '\r' || ch == '\n' || ch == ' ') {
                if (vplay.selected_file < vplay.file_count) {
                    video_load_gif(vplay.files[vplay.selected_file].path);
                    video_render(ctx);
                }
                return true;
            }
            if (ch == 'r' || ch == 'R') {
                video_refresh_file_list();
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
                solar_os_context_request_exit(ctx);
                return true;
            }
        } else {
            /* Playback Controls */
            if (ch == ' ' || ch == '\r' || ch == '\n') {
                vplay.playing = !vplay.playing;
                vplay.osd_until_ms = (uint32_t)(now_us / 1000) + 3000;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A') {
                if (vplay.frame_index > 0) vplay.frame_index--;
                else vplay.frame_index = vplay.frame_count - 1;
                vplay.playing = false;
                vplay.osd_until_ms = (uint32_t)(now_us / 1000) + 3000;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D') {
                vplay.frame_index = (vplay.frame_index + 1) % vplay.frame_count;
                vplay.playing = false;
                vplay.osd_until_ms = (uint32_t)(now_us / 1000) + 3000;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
                if (vplay.speed < 2.5f) vplay.speed += 0.25f;
                vplay.osd_until_ms = (uint32_t)(now_us / 1000) + 3000;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
                if (vplay.speed > 0.35f) vplay.speed -= 0.25f;
                vplay.osd_until_ms = (uint32_t)(now_us / 1000) + 3000;
                video_render(ctx);
                return true;
            }
            if (ch == 'l' || ch == 'L') {
                vplay.loop = !vplay.loop;
                vplay.osd_until_ms = (uint32_t)(now_us / 1000) + 3000;
                video_render(ctx);
                return true;
            }
            if (ch == 'i' || ch == 'I') {
                vplay.show_osd = !vplay.show_osd;
                video_render(ctx);
                return true;
            }
            if (ch == 'r' || ch == 'R') {
                vplay.frame_index = 0;
                vplay.next_frame_us = now_us;
                vplay.osd_until_ms = (uint32_t)(now_us / 1000) + 3000;
                video_render(ctx);
                return true;
            }
            if (ch == 'o' || ch == 'O') {
                vplay.in_picker = true;
                video_refresh_file_list();
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
                vplay.in_picker = true;
                video_refresh_file_list();
                video_render(ctx);
                return true;
            }
        }
    }

    return false;
}

const solar_os_app_t solar_os_video_player_app = {
    .name = "video_player",
    .summary = "cinema video and animated GIF player",
    .flags = 0,
    .start = video_start,
    .stop = video_stop,
    .event = video_event,
    .state_slot = &video_state_ptr,
    .state_size = sizeof(video_player_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 15U,
    .worker_stack_bytes = VIDEO_STACK_SIZE,
};
