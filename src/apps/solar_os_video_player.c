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
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_resource_limits.h"
#include "solar_os_stb_image.h"
#include "solar_os_storage.h"
#include "solar_os_vector.h"

#define VIDEO_STACK_SIZE 24576
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(VIDEO_STACK_SIZE);

#define VIDEO_MAX_FILES 64
#define VIDEO_MAX_CANVAS_PIXELS (512U * 512U)
#define VIDEO_MAX_STORED_PIXELS (4U * 1024U * 1024U)
#define VIDEO_MJPEG_BUF_SIZE (64U * 1024U)

static const char *TAG = "solar_os_video_player";

typedef enum {
    MEDIA_TYPE_NONE = 0,
    MEDIA_TYPE_GIF,
    MEDIA_TYPE_MJPEG,
    MEDIA_TYPE_SLV,
    MEDIA_TYPE_IMAGE,
} video_media_type_t;

typedef enum {
    LOOP_MODE_ONE = 0,
    LOOP_MODE_ALL = 1,
    LOOP_MODE_OFF = 2,
} video_loop_mode_t;

typedef struct {
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char name[48];
    video_media_type_t type;
    bool has_audio;
} video_file_entry_t;

typedef struct {
    char magic[4];       /* "SLV1" */
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    uint8_t reserved;
    uint32_t frame_count;
} __attribute__((packed)) slv_header_t;

typedef struct {
    video_media_type_t type;
    FILE *stream_file;
    uint8_t *stream_buf;
    size_t stream_buf_size;
    
    /* Single current frame buffer */
    uint8_t *cur_frame_gray;
    bool cur_frame_is_stb;
    uint32_t width;
    uint32_t height;
    uint32_t frame_count;
    uint32_t frame_index;
    
    /* GIF specific in-RAM animation */
    uint8_t *gif_all_gray;
    uint32_t *gif_delays_ms;

    /* Timing & Playback */
    int64_t next_frame_us;
    uint32_t frame_delay_ms;
    float speed;
    bool playing;
    video_loop_mode_t loop_mode;
    bool show_osd;
    uint32_t osd_until_ms;
    
    /* Audio */
    bool has_audio;
    char audio_path[SOLAR_OS_STORAGE_PATH_MAX];

    /* Browser */
    bool in_picker;
    char current_path[SOLAR_OS_STORAGE_PATH_MAX];
    char status_msg[64];
    video_file_entry_t files[VIDEO_MAX_FILES];
    size_t file_count;
    size_t selected_file;
} video_player_state_t;

static void *video_state_ptr;
#define vplay (*(video_player_state_t *)video_state_ptr)

static inline solar_os_gfx_color_t video_gray_to_color(uint8_t gray)
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
    if (vplay.stream_file != NULL) {
        fclose(vplay.stream_file);
        vplay.stream_file = NULL;
    }
    if (vplay.stream_buf != NULL) {
        solar_os_memory_free(vplay.stream_buf);
        vplay.stream_buf = NULL;
    }
    if (vplay.cur_frame_gray != NULL) {
        if (vplay.cur_frame_is_stb) {
            solar_os_stb_image_free(vplay.cur_frame_gray);
        } else {
            solar_os_memory_free(vplay.cur_frame_gray);
        }
        vplay.cur_frame_gray = NULL;
        vplay.cur_frame_is_stb = false;
    }
    if (vplay.gif_all_gray != NULL) {
        solar_os_stb_image_free(vplay.gif_all_gray);
        vplay.gif_all_gray = NULL;
    }
    if (vplay.gif_delays_ms != NULL) {
        free(vplay.gif_delays_ms);
        vplay.gif_delays_ms = NULL;
    }
    vplay.type = MEDIA_TYPE_NONE;
    vplay.width = 0;
    vplay.height = 0;
    vplay.frame_count = 0;
    vplay.frame_index = 0;
    vplay.has_audio = false;
    vplay.audio_path[0] = '\0';
}

static esp_err_t video_load_media(const char *path);

static void video_handle_playback_end(void)
{
    if (vplay.loop_mode == LOOP_MODE_ONE) {
        vplay.frame_index = 0;
        if (vplay.stream_file != NULL) {
            fseek(vplay.stream_file, (vplay.type == MEDIA_TYPE_SLV) ? (long)sizeof(slv_header_t) : 0, SEEK_SET);
        }
    } else if (vplay.loop_mode == LOOP_MODE_ALL) {
        if (vplay.file_count > 1) {
            vplay.selected_file = (vplay.selected_file + 1) % vplay.file_count;
            video_load_media(vplay.files[vplay.selected_file].path);
        } else {
            vplay.frame_index = 0;
            if (vplay.stream_file != NULL) {
                fseek(vplay.stream_file, (vplay.type == MEDIA_TYPE_SLV) ? (long)sizeof(slv_header_t) : 0, SEEK_SET);
            }
        }
    } else {
        vplay.playing = false;
    }
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
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "File too large (%ld KB)", fsize / 1024);
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
        vplay.type = MEDIA_TYPE_GIF;
        vplay.width = anim.width;
        vplay.height = anim.height;
        vplay.frame_count = anim.frame_count;
        vplay.gif_all_gray = anim.gray;
        vplay.gif_delays_ms = anim.delays_ms;
        vplay.frame_index = 0;
        vplay.playing = true;
        vplay.speed = 1.0f;
        vplay.frame_delay_ms = (anim.delays_ms != NULL && anim.delays_ms[0] > 0) ? anim.delays_ms[0] : 66;
        vplay.next_frame_us = esp_timer_get_time();
        vplay.in_picker = false;
        vplay.show_osd = true;
        vplay.osd_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 3000;
        strlcpy(vplay.current_path, path, sizeof(vplay.current_path));
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Loaded %" PRIu32 " frames (%" PRIu32 "x%" PRIu32 ")",
                 vplay.frame_count, vplay.width, vplay.height);
        return ESP_OK;
    }

    solar_os_stb_gif_animation_free(&anim);
    snprintf(vplay.status_msg, sizeof(vplay.status_msg), "GIF decode failed");
    return err;
}

static esp_err_t video_read_next_mjpeg_frame(void)
{
    if (vplay.stream_file == NULL || vplay.stream_buf == NULL) return ESP_FAIL;

    /* Fast chunked scan for JPEG SOI (0xFF 0xD8) to EOI (0xFF 0xD9) */
    size_t jpeg_len = 0;
    bool found_soi = false;
    uint8_t read_chunk[1024];

    while (1) {
        size_t n = fread(read_chunk, 1, sizeof(read_chunk), vplay.stream_file);
        if (n == 0) {
            /* End of file reached */
            video_handle_playback_end();
            return ESP_OK;
        }

        for (size_t i = 0; i < n; i++) {
            uint8_t b = read_chunk[i];
            if (!found_soi) {
                if (b == 0xFF && i + 1 < n && read_chunk[i + 1] == 0xD8) {
                    found_soi = true;
                    vplay.stream_buf[0] = 0xFF;
                    vplay.stream_buf[1] = 0xD8;
                    jpeg_len = 2;
                    i++;
                }
            } else {
                if (jpeg_len < vplay.stream_buf_size) {
                    vplay.stream_buf[jpeg_len++] = b;
                }
                if (b == 0xD9 && jpeg_len > 3 && vplay.stream_buf[jpeg_len - 2] == 0xFF) {
                    /* Found complete JPEG frame! Seek back unread portion */
                    long unread = (long)(n - 1 - i);
                    if (unread > 0) {
                        fseek(vplay.stream_file, -unread, SEEK_CUR);
                    }
                    goto decode_frame;
                }
            }
        }
    }

decode_frame:
    if (jpeg_len > 4) {
        uint8_t *decoded_gray = NULL;
        uint32_t dw = 0, dh = 0;
        esp_err_t err = solar_os_stb_jpeg_decode_gray(vplay.stream_buf, jpeg_len,
                                                      VIDEO_MAX_CANVAS_PIXELS,
                                                      &decoded_gray, &dw, &dh);
        if (err == ESP_OK && decoded_gray != NULL) {
            if (vplay.cur_frame_gray != NULL) {
                if (vplay.cur_frame_is_stb) {
                    solar_os_stb_image_free(vplay.cur_frame_gray);
                } else {
                    solar_os_memory_free(vplay.cur_frame_gray);
                }
            }
            vplay.cur_frame_gray = decoded_gray;
            vplay.cur_frame_is_stb = true;
            vplay.width = dw;
            vplay.height = dh;
            vplay.frame_index++;
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

static esp_err_t video_load_mjpeg(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Cannot open MJPEG file");
        return ESP_FAIL;
    }

    video_free_media();
    vplay.stream_file = f;
    vplay.stream_buf_size = VIDEO_MJPEG_BUF_SIZE;
    vplay.stream_buf = solar_os_memory_alloc(vplay.stream_buf_size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "mjpeg.buf");
    if (vplay.stream_buf == NULL) {
        fclose(f);
        vplay.stream_file = NULL;
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    vplay.type = MEDIA_TYPE_MJPEG;
    vplay.frame_delay_ms = 50; /* 20 FPS */
    vplay.speed = 1.0f;
    vplay.playing = true;
    vplay.frame_index = 0;

    /* Check for companion .wav audio */
    char wav_path[SOLAR_OS_STORAGE_PATH_MAX];
    strlcpy(wav_path, path, sizeof(wav_path));
    char *dot = strrchr(wav_path, '.');
    if (dot != NULL) {
        strlcpy(dot, ".wav", sizeof(wav_path) - (size_t)(dot - wav_path));
        FILE *fwav = fopen(wav_path, "rb");
        if (fwav != NULL) {
            fclose(fwav);
            vplay.has_audio = true;
            strlcpy(vplay.audio_path, wav_path, sizeof(vplay.audio_path));
        }
    }

    esp_err_t err = video_read_next_mjpeg_frame();
    if (err == ESP_OK) {
        vplay.in_picker = false;
        vplay.show_osd = true;
        vplay.osd_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 3000;
        strlcpy(vplay.current_path, path, sizeof(vplay.current_path));
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Streaming MJPEG (%\" PRIu32 \"x%\" PRIu32 \")", vplay.width, vplay.height);
        return ESP_OK;
    }

    video_free_media();
    snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Failed to read MJPEG");
    return ESP_FAIL;
}

static esp_err_t video_read_next_slv_frame(void)
{
    if (vplay.stream_file == NULL || vplay.cur_frame_gray == NULL) return ESP_FAIL;

    const size_t packed_bytes = (size_t)(vplay.width * vplay.height) / 4U;
    if (vplay.stream_buf == NULL || vplay.stream_buf_size < packed_bytes) return ESP_FAIL;

    size_t read_bytes = fread(vplay.stream_buf, 1, packed_bytes, vplay.stream_file);
    if (read_bytes < packed_bytes) {
        video_handle_playback_end();
        return ESP_OK;
    }

    /* Unpack 2-bit frame into grayscale buffer */
    const uint8_t *src = vplay.stream_buf;
    uint8_t *dst = vplay.cur_frame_gray;
    const uint8_t palette[4] = {255, 170, 85, 0};

    const size_t total_pixels = (size_t)vplay.width * vplay.height;
    size_t p = 0;
    for (size_t i = 0; i < packed_bytes && p + 3 < total_pixels; i++) {
        uint8_t b = src[i];
        dst[p++] = palette[(b >> 6) & 0x03];
        dst[p++] = palette[(b >> 4) & 0x03];
        dst[p++] = palette[(b >> 2) & 0x03];
        dst[p++] = palette[b & 0x03];
    }

    vplay.frame_index++;
    return ESP_OK;
}

static esp_err_t video_load_slv(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Cannot open video file");
        return ESP_FAIL;
    }

    slv_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr.magic, "SLV1", 4) != 0) {
        fclose(f);
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Invalid SLV header");
        return ESP_FAIL;
    }

    video_free_media();
    vplay.stream_file = f;
    vplay.width = hdr.width;
    vplay.height = hdr.height;
    vplay.frame_count = hdr.frame_count;
    vplay.frame_delay_ms = (hdr.fps > 0) ? (1000 / hdr.fps) : 50;
    vplay.speed = 1.0f;
    vplay.playing = true;
    vplay.frame_index = 0;

    const size_t packed_bytes = (size_t)(vplay.width * vplay.height) / 4U;
    vplay.stream_buf_size = packed_bytes;
    vplay.stream_buf = solar_os_memory_alloc(packed_bytes, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "slv.packed");
    vplay.cur_frame_gray = solar_os_memory_alloc((size_t)vplay.width * vplay.height, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "slv.frame");
    vplay.cur_frame_is_stb = false;

    if (vplay.stream_buf == NULL || vplay.cur_frame_gray == NULL) {
        video_free_media();
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    vplay.type = MEDIA_TYPE_SLV;
    esp_err_t err = video_read_next_slv_frame();
    if (err == ESP_OK) {
        vplay.in_picker = false;
        vplay.show_osd = true;
        vplay.osd_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 3000;
        strlcpy(vplay.current_path, path, sizeof(vplay.current_path));
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Streaming SLV (%" PRIu32 "x%" PRIu32 ", %u FPS)",
                 vplay.width, vplay.height, (unsigned)hdr.fps);
        return ESP_OK;
    }

    video_free_media();
    snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Failed to read frame");
    return ESP_FAIL;
}

static esp_err_t video_load_media(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    const char *dot = strrchr(path, '.');
    if (dot == NULL) return ESP_ERR_INVALID_ARG;

    if (strcasecmp(dot, ".gif") == 0) {
        return video_load_gif(path);
    } else if (strcasecmp(dot, ".mjpeg") == 0 || strcasecmp(dot, ".mjpg") == 0) {
        return video_load_mjpeg(path);
    } else if (strcasecmp(dot, ".slv") == 0 || strcasecmp(dot, ".flv") == 0 || strcasecmp(dot, ".vid") == 0) {
        return video_load_slv(path);
    }

    return ESP_ERR_NOT_SUPPORTED;
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

        video_media_type_t t = MEDIA_TYPE_NONE;
        if (strcasecmp(dot, ".gif") == 0) t = MEDIA_TYPE_GIF;
        else if (strcasecmp(dot, ".mjpeg") == 0 || strcasecmp(dot, ".mjpg") == 0) t = MEDIA_TYPE_MJPEG;
        else if (strcasecmp(dot, ".slv") == 0 || strcasecmp(dot, ".flv") == 0 || strcasecmp(dot, ".vid") == 0) t = MEDIA_TYPE_SLV;
        else if (strcasecmp(dot, ".bmp") == 0 || strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".png") == 0) t = MEDIA_TYPE_IMAGE;

        if (t != MEDIA_TYPE_NONE && vplay.file_count < VIDEO_MAX_FILES) {
            video_file_entry_t *e = &vplay.files[vplay.file_count];
            snprintf(e->path, sizeof(e->path), "%s/%s", dir_path, entry->d_name);
            strlcpy(e->name, entry->d_name, sizeof(e->name));
            e->type = t;
            
            /* Check if companion .wav audio exists */
            char wav_test[SOLAR_OS_STORAGE_PATH_MAX];
            strlcpy(wav_test, e->path, sizeof(wav_test));
            char *d = strrchr(wav_test, '.');
            if (d != NULL) {
                strlcpy(d, ".wav", sizeof(wav_test) - (size_t)(d - wav_test));
                FILE *fw = fopen(wav_test, "r");
                if (fw != NULL) {
                    fclose(fw);
                    e->has_audio = true;
                }
            }

            vplay.file_count++;
        }
    }
    closedir(dir);
}

static void video_refresh_file_list(void)
{
    vplay.file_count = 0;
    vplay.selected_file = 0;

    video_scan_folder("/sdcard/videos");
    video_scan_folder("/sdcard/gifs");
    video_scan_folder("/sdcard/photos");
    video_scan_folder("/sdcard");
    video_scan_folder("/flash/videos");
    video_scan_folder("/flash/gifs");
}

static void video_draw_frame(solar_os_gfx_t *gfx, int screen_w, int screen_h)
{
    const uint8_t *cur_gray = NULL;
    if (vplay.type == MEDIA_TYPE_GIF && vplay.gif_all_gray != NULL) {
        const size_t frame_bytes = (size_t)vplay.width * vplay.height;
        cur_gray = vplay.gif_all_gray + ((size_t)vplay.frame_index * frame_bytes);
    } else if ((vplay.type == MEDIA_TYPE_MJPEG || vplay.type == MEDIA_TYPE_SLV) && vplay.cur_frame_gray != NULL) {
        cur_gray = vplay.cur_frame_gray;
    }

    if (cur_gray == NULL || vplay.width == 0 || vplay.height == 0) return;

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
        solar_os_gfx_text(gfx, 8, 16, "SOLAROS CINEMA VIDEO & GIF PLAYER");

        char count_txt[32];
        snprintf(count_txt, sizeof(count_txt), "%u media files", (unsigned)vplay.file_count);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        const size_t cw = solar_os_gfx_text_width(gfx, count_txt);
        solar_os_gfx_text(gfx, screen_w - (int)cw - 8, 16, count_txt);

        if (vplay.file_count == 0) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
            solar_os_gfx_text(gfx, 30, 90, "No media files found on SD Card.");
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 30, 115, "Formats supported:");
            solar_os_gfx_text(gfx, 40, 135, "- .mjpeg : Motion JPEG Video (streaming, unlimited duration)");
            solar_os_gfx_text(gfx, 40, 155, "- .slv   : SolarOS Ultra-Fast 2-bit Video (30-50 FPS)");
            solar_os_gfx_text(gfx, 40, 175, "- .gif   : Animated GIF (short clips <= 5s, <= 4 MB)");
            solar_os_gfx_text(gfx, 40, 195, "- .wav   : Companion audio for .mjpeg (auto-detected)");
            solar_os_gfx_text(gfx, 30, 225, "Use the PC Converter GUI in tools/ to convert any video!");
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
                const char *type_lbl = "[MEDIA]";
                if (vplay.files[idx].type == MEDIA_TYPE_GIF) type_lbl = "[GIF]";
                else if (vplay.files[idx].type == MEDIA_TYPE_MJPEG) type_lbl = vplay.files[idx].has_audio ? "[MJPEG + AUDIO]" : "[MJPEG VIDEO]";
                else if (vplay.files[idx].type == MEDIA_TYPE_SLV) type_lbl = "[SLV STREAM]";
                
                const size_t tw = solar_os_gfx_text_width(gfx, type_lbl);
                solar_os_gfx_text(gfx, screen_w - (int)tw - 18, y + 19, type_lbl);
            }
        }

        /* Footer */
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
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
        const char *fmt_str = (vplay.type == MEDIA_TYPE_MJPEG) ? "MJPEG" : ((vplay.type == MEDIA_TYPE_SLV) ? "SLV" : "GIF");
        const char *loop_str = (vplay.loop_mode == LOOP_MODE_ONE) ? "LOOP 1" : ((vplay.loop_mode == LOOP_MODE_ALL) ? "LOOP ALL" : "LOOP OFF");
        snprintf(meta_txt, sizeof(meta_txt), "%s | %s | %.1fx%s",
                 fmt_str, loop_str, vplay.speed, vplay.has_audio ? " | AUDIO" : "");
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
        if (vplay.frame_count > 1) {
            snprintf(ctrl_txt, sizeof(ctrl_txt), "[%s] Frame %\" PRIu32 \"/%\" PRIu32 \" | [TAB/L] %s | [SPACE] Play/Pause",
                     vplay.playing ? "PLAY" : "PAUSE", vplay.frame_index + 1, vplay.frame_count, loop_str);
        } else {
            snprintf(ctrl_txt, sizeof(ctrl_txt), "[%s] Frame %\" PRIu32 \" | [TAB/L] %s | [SPACE] Play/Pause | [ESC] Exit",
                     vplay.playing ? "PLAY" : "PAUSE", vplay.frame_index + 1, loop_str);
        }
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 8, screen_h - 4, ctrl_txt);
    }

    solar_os_gfx_present(gfx);
}

static esp_err_t video_start(solar_os_context_t *ctx)
{
    memset(&vplay, 0, sizeof(vplay));
    vplay.speed = 1.0f;
    vplay.loop_mode = LOOP_MODE_ONE;
    vplay.playing = true;

    solar_os_context_set_graphics_active(ctx, true);

    const int argc = solar_os_context_argc(ctx);
    if (argc > 1) {
        const char *launch_path = solar_os_context_argv(ctx, 1);
        if (launch_path != NULL && launch_path[0] != '\0') {
            esp_err_t err = video_load_media(launch_path);
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
        if (!vplay.in_picker && vplay.playing) {
            if (now_us >= vplay.next_frame_us) {
                if (vplay.type == MEDIA_TYPE_GIF && vplay.gif_all_gray != NULL && vplay.frame_count > 1) {
                    vplay.frame_index = (vplay.frame_index + 1) % vplay.frame_count;
                    uint32_t delay_ms = (vplay.gif_delays_ms != NULL && vplay.gif_delays_ms[vplay.frame_index] > 0) ?
                                        vplay.gif_delays_ms[vplay.frame_index] : 66;
                    delay_ms = (uint32_t)((float)delay_ms / vplay.speed);
                    if (delay_ms < 10) delay_ms = 10;
                    vplay.next_frame_us = now_us + (int64_t)delay_ms * 1000LL;
                    video_render(ctx);
                } else if (vplay.type == MEDIA_TYPE_MJPEG && vplay.stream_file != NULL) {
                    if (video_read_next_mjpeg_frame() == ESP_OK) {
                        uint32_t delay_ms = (uint32_t)((float)vplay.frame_delay_ms / vplay.speed);
                        if (delay_ms < 10) delay_ms = 10;
                        vplay.next_frame_us = now_us + (int64_t)delay_ms * 1000LL;
                        video_render(ctx);
                    }
                } else if (vplay.type == MEDIA_TYPE_SLV && vplay.stream_file != NULL) {
                    if (video_read_next_slv_frame() == ESP_OK) {
                        uint32_t delay_ms = (uint32_t)((float)vplay.frame_delay_ms / vplay.speed);
                        if (delay_ms < 10) delay_ms = 10;
                        vplay.next_frame_us = now_us + (int64_t)delay_ms * 1000LL;
                        video_render(ctx);
                    }
                }
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
                    video_load_media(vplay.files[vplay.selected_file].path);
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
            if (ch == '\t' || ch == 'l' || ch == 'L') {
                /* Cycle loop modes: ONE -> ALL -> OFF */
                if (vplay.loop_mode == LOOP_MODE_ONE) vplay.loop_mode = LOOP_MODE_ALL;
                else if (vplay.loop_mode == LOOP_MODE_ALL) vplay.loop_mode = LOOP_MODE_OFF;
                else vplay.loop_mode = LOOP_MODE_ONE;

                vplay.osd_until_ms = (uint32_t)(now_us / 1000) + 3000;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A') {
                if (vplay.type == MEDIA_TYPE_GIF && vplay.frame_count > 1) {
                    if (vplay.frame_index > 0) vplay.frame_index--;
                    else vplay.frame_index = vplay.frame_count - 1;
                }
                vplay.playing = false;
                vplay.osd_until_ms = (uint32_t)(now_us / 1000) + 3000;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D') {
                if (vplay.type == MEDIA_TYPE_GIF && vplay.frame_count > 1) {
                    vplay.frame_index = (vplay.frame_index + 1) % vplay.frame_count;
                }
                vplay.playing = false;
                vplay.osd_until_ms = (uint32_t)(now_us / 1000) + 3000;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
                if (vplay.speed < 3.0f) vplay.speed += 0.25f;
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
            if (ch == 'i' || ch == 'I') {
                vplay.show_osd = !vplay.show_osd;
                video_render(ctx);
                return true;
            }
            if (ch == 'r' || ch == 'R') {
                vplay.frame_index = 0;
                if (vplay.stream_file != NULL) {
                    fseek(vplay.stream_file, (vplay.type == MEDIA_TYPE_SLV) ? (long)sizeof(slv_header_t) : 0, SEEK_SET);
                }
                vplay.next_frame_us = now_us;
                vplay.osd_until_ms = (uint32_t)(now_us / 1000) + 3000;
                video_render(ctx);
                return true;
            }
            if (ch == 'o' || ch == 'O' || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
                video_free_media();
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
    .tick_interval_ms = 10U,
    .worker_stack_bytes = VIDEO_STACK_SIZE,
};
