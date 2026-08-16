#include "solar_os_video_player.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_audio.h"
#include "solar_os_display.h"
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
#define VIDEO_CANVAS_WIDTH 400U
#define VIDEO_CANVAS_HEIGHT 300U
#define VIDEO_XBM_STRIDE (VIDEO_CANVAS_WIDTH / 8U) /* 50 bytes */
#define VIDEO_XBM_BYTES (VIDEO_XBM_STRIDE * VIDEO_CANVAS_HEIGHT) /* 15,000 bytes */
#define VIDEO_MAX_CANVAS_PIXELS (VIDEO_CANVAS_WIDTH * VIDEO_CANVAS_HEIGHT)
#define VIDEO_MAX_STORED_PIXELS (2U * 1024U * 1024U)
#define VIDEO_MJPEG_BUF_SIZE (128U * 1024U)

static const char *TAG = "solar_os_video_player";

typedef enum {
    MEDIA_TYPE_NONE = 0,
    MEDIA_TYPE_GIF,
    MEDIA_TYPE_MJPEG,
    MEDIA_TYPE_SLV,
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
    
    /* High-speed 1-bit DMA display buffer */
    uint8_t *xbm_buf;

    /* GIF animation structures */
    solar_os_stb_gif_animation_t gif_anim;

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

    /* Display Quality & Dithering */
    uint8_t dither_mode; /* 0 = Clean Contrast Bayer, 1 = Standard Bayer, 2 = Sharp Pure B&W */

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

static void video_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("[VID_DEBUG] %s\n", buf);

    if (solar_os_storage_sd_is_mounted()) {
        const char *sd = solar_os_storage_sd_mount_point();
        char log_path[64];
        snprintf(log_path, sizeof(log_path), "%s/solar_os_crash.log", sd ? sd : "/sdcard");
        FILE *flog = fopen(log_path, "a");
        if (flog != NULL) {
            fprintf(flog, "[%u ms] %s\n", (unsigned)(esp_timer_get_time() / 1000), buf);
            fclose(flog);
        }
    }
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
    if (vplay.gif_anim.gray != NULL || vplay.gif_anim.delays_ms != NULL) {
        solar_os_stb_gif_animation_free(&vplay.gif_anim);
        memset(&vplay.gif_anim, 0, sizeof(vplay.gif_anim));
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
    video_log("Loading GIF: %s", path);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        video_log("ERROR: Cannot open GIF file: %s", path);
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Cannot open GIF");
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 6 * 1024 * 1024) {
        fclose(f);
        video_log("ERROR: GIF too large (%ld KB)", fsize / 1024);
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "File too large (%ld KB)", fsize / 1024);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *raw_buf = solar_os_memory_alloc((size_t)fsize, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "video.raw");
    if (raw_buf == NULL) {
        fclose(f);
        video_log("ERROR: Out of memory allocating raw buffer (%ld bytes)", fsize);
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    if (fread(raw_buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        solar_os_memory_free(raw_buf);
        fclose(f);
        video_log("ERROR: Read error from GIF file");
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Read error");
        return ESP_FAIL;
    }
    fclose(f);

    video_free_media();

    esp_err_t err = solar_os_stb_decode_gif_gray(raw_buf,
                                                (size_t)fsize,
                                                VIDEO_MAX_CANVAS_PIXELS,
                                                VIDEO_MAX_STORED_PIXELS,
                                                400,
                                                300,
                                                solar_os_vector_rgba_to_gray_scaled,
                                                &vplay.gif_anim);
    solar_os_memory_free(raw_buf);

    if (err == ESP_OK && vplay.gif_anim.gray != NULL && vplay.gif_anim.frame_count > 0) {
        vplay.type = MEDIA_TYPE_GIF;
        vplay.width = vplay.gif_anim.width;
        vplay.height = vplay.gif_anim.height;
        vplay.frame_count = vplay.gif_anim.frame_count;
        vplay.frame_index = 0;
        vplay.playing = true;
        vplay.speed = 1.0f;
        vplay.frame_delay_ms = (vplay.gif_anim.delays_ms != NULL && vplay.gif_anim.delays_ms[0] > 0) ? vplay.gif_anim.delays_ms[0] : 66;
        vplay.next_frame_us = esp_timer_get_time();
        vplay.in_picker = false;
        vplay.show_osd = true;
        vplay.osd_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 3000;
        strlcpy(vplay.current_path, path, sizeof(vplay.current_path));
        solar_os_display_set_high_refresh_override("display0", true, 255U);
        video_log("GIF loaded: %u frames (%ux%u)", (unsigned)vplay.frame_count, (unsigned)vplay.width, (unsigned)vplay.height);
        return ESP_OK;
    }

    video_free_media();
    video_log("ERROR: GIF decode failed (err=%s)", esp_err_to_name(err));
    snprintf(vplay.status_msg, sizeof(vplay.status_msg), "GIF decode failed");
    return err;
}

static esp_err_t video_read_next_mjpeg_frame(void)
{
    if (vplay.stream_file == NULL || vplay.stream_buf == NULL) return ESP_FAIL;

    size_t jpeg_len = 0;
    int state = 0; /* 0: search 0xFF, 1: search 0xD8, 2: in payload, 3: saw 0xFF */
    uint8_t chunk[2048];
    size_t loop_count = 0;

    while (loop_count++ < 100) {
        size_t n = fread(chunk, 1, sizeof(chunk), vplay.stream_file);
        if (n == 0) {
            video_handle_playback_end();
            return ESP_OK;
        }

        for (size_t i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            
            if (state == 0) {
                if (b == 0xFF) state = 1;
            } else if (state == 1) {
                if (b == 0xD8) {
                    vplay.stream_buf[0] = 0xFF;
                    vplay.stream_buf[1] = 0xD8;
                    jpeg_len = 2;
                    state = 2;
                } else if (b != 0xFF) {
                    state = 0;
                }
            } else if (state == 2) {
                if (jpeg_len < vplay.stream_buf_size) {
                    vplay.stream_buf[jpeg_len++] = b;
                }
                if (b == 0xFF) {
                    state = 3;
                }
            } else if (state == 3) {
                if (jpeg_len < vplay.stream_buf_size) {
                    vplay.stream_buf[jpeg_len++] = b;
                }
                if (b == 0xD9) {
                    long unread = (long)(n - 1 - i);
                    if (unread > 0) {
                        fseek(vplay.stream_file, -unread, SEEK_CUR);
                    }
                    goto decode_frame;
                } else if (b != 0xFF) {
                    state = 2;
                }
            }
        }
    }

decode_frame:
    if (jpeg_len > 4 && vplay.stream_buf[0] == 0xFF && vplay.stream_buf[1] == 0xD8) {
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
    video_log("Loading MJPEG: %s", path);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        video_log("ERROR: Cannot open MJPEG file: %s (errno=%d)", path, errno);
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Cannot open MJPEG");
        return ESP_FAIL;
    }

    video_free_media();
    vplay.stream_file = f;
    vplay.stream_buf_size = VIDEO_MJPEG_BUF_SIZE;
    vplay.stream_buf = solar_os_memory_alloc(vplay.stream_buf_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "mjpeg.buf");
    if (vplay.stream_buf == NULL) {
        fclose(f);
        vplay.stream_file = NULL;
        video_log("ERROR: Out of memory for MJPEG buffer");
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    vplay.type = MEDIA_TYPE_MJPEG;
    vplay.frame_delay_ms = 45; /* ~22 FPS */
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
            video_log("Found companion audio: %s", wav_path);
        }
    }

    esp_err_t err = video_read_next_mjpeg_frame();
    if (err == ESP_OK) {
        vplay.in_picker = false;
        vplay.show_osd = true;
        vplay.osd_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 3000;
        strlcpy(vplay.current_path, path, sizeof(vplay.current_path));
        solar_os_display_set_high_refresh_override("display0", true, 255U);
        video_log("MJPEG loaded successfully (%ux%u)", (unsigned)vplay.width, (unsigned)vplay.height);
        return ESP_OK;
    }

    video_free_media();
    video_log("ERROR: Failed to read first MJPEG frame");
    snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Read MJPEG failed");
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

    /* Unpack 2-bit frame into grayscale: 0=Black(0), 1=Dark(85), 2=Light(170), 3=White(255) */
    const uint8_t *src = vplay.stream_buf;
    uint8_t *dst = vplay.cur_frame_gray;
    static const uint8_t palette[4] = {0, 85, 170, 255};

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
    video_log("Loading SLV: %s", path);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        video_log("ERROR: Cannot open SLV file: %s (errno=%d)", path, errno);
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Cannot open video file");
        return ESP_FAIL;
    }

    slv_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr.magic, "SLV1", 4) != 0) {
        fclose(f);
        video_log("ERROR: Invalid SLV magic header in %s", path);
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Invalid SLV header");
        return ESP_FAIL;
    }

    video_log("SLV dimensions: %ux%u @ %u FPS, %u frames", hdr.width, hdr.height, (unsigned)hdr.fps, (unsigned)hdr.frame_count);

    if (hdr.width == 0 || hdr.height == 0 || hdr.width > 400 || hdr.height > 300) {
        fclose(f);
        video_log("ERROR: Invalid SLV dimensions %ux%u", hdr.width, hdr.height);
        snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Invalid SLV dimensions (%ux%u)", hdr.width, hdr.height);
        return ESP_ERR_INVALID_SIZE;
    }

    video_free_media();
    vplay.stream_file = f;
    vplay.width = hdr.width;
    vplay.height = hdr.height;
    vplay.frame_count = hdr.frame_count;
    vplay.frame_delay_ms = (hdr.fps > 0) ? (1000 / hdr.fps) : 40;
    vplay.speed = 1.0f;
    vplay.playing = true;
    vplay.frame_index = 0;

    const size_t packed_bytes = (size_t)(vplay.width * vplay.height) / 4U;
    vplay.stream_buf_size = packed_bytes;
    vplay.stream_buf = solar_os_memory_alloc(packed_bytes, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "slv.packed");
    vplay.cur_frame_gray = solar_os_memory_alloc((size_t)vplay.width * vplay.height, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "slv.frame");
    vplay.cur_frame_is_stb = false;

    if (vplay.stream_buf == NULL || vplay.cur_frame_gray == NULL) {
        video_free_media();
        video_log("ERROR: Out of memory allocating SLV frame buffers");
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
        solar_os_display_set_high_refresh_override("display0", true, 255U);
        video_log("SLV loaded successfully: %s", path);
        return ESP_OK;
    }

    video_free_media();
    video_log("ERROR: Failed to read initial SLV frame");
    snprintf(vplay.status_msg, sizeof(vplay.status_msg), "Failed to read frame");
    return ESP_FAIL;
}

static esp_err_t video_load_media(const char *path)
{
    video_log("video_load_media: %s (Free PSRAM: %u KB, Free Heap: %u KB)",
              path ? path : "NULL",
              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
              (unsigned)(esp_get_free_heap_size() / 1024));

    if (path == NULL || path[0] == '\0') return ESP_ERR_INVALID_ARG;

    char resolved[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        strlcpy(resolved, path, sizeof(resolved));
    }

    const char *dot = strrchr(resolved, '.');
    if (dot == NULL) return ESP_ERR_INVALID_ARG;

    if (strcasecmp(dot, ".slv") == 0 || strcasecmp(dot, ".flv") == 0 || strcasecmp(dot, ".vid") == 0) {
        return video_load_slv(resolved);
    } else if (strcasecmp(dot, ".mjpeg") == 0 || strcasecmp(dot, ".mjpg") == 0) {
        return video_load_mjpeg(resolved);
    } else if (strcasecmp(dot, ".gif") == 0) {
        return video_load_gif(resolved);
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
        if (strcasecmp(dot, ".slv") == 0 || strcasecmp(dot, ".flv") == 0 || strcasecmp(dot, ".vid") == 0) t = MEDIA_TYPE_SLV;
        else if (strcasecmp(dot, ".mjpeg") == 0 || strcasecmp(dot, ".mjpg") == 0) t = MEDIA_TYPE_MJPEG;
        else if (strcasecmp(dot, ".gif") == 0) t = MEDIA_TYPE_GIF;

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

    const char *sd = solar_os_storage_sd_is_mounted() ? solar_os_storage_sd_mount_point() : "/sdcard";
    const char *flash = solar_os_storage_flash_is_mounted() ? solar_os_storage_flash_mount_point() : "/flash";

    char p[128];
    if (sd != NULL) {
        snprintf(p, sizeof(p), "%s/videos", sd); video_scan_folder(p);
        snprintf(p, sizeof(p), "%s/gifs", sd); video_scan_folder(p);
        video_scan_folder(sd);
    }
    if (flash != NULL) {
        snprintf(p, sizeof(p), "%s/videos", flash); video_scan_folder(p);
        snprintf(p, sizeof(p), "%s/gifs", flash); video_scan_folder(p);
        video_scan_folder(flash);
    }
}

static void video_play_next(void)
{
    if (vplay.file_count <= 1) return;
    vplay.selected_file = (vplay.selected_file + 1) % vplay.file_count;
    video_load_media(vplay.files[vplay.selected_file].path);
}

static void video_play_prev(void)
{
    if (vplay.file_count <= 1) return;
    if (vplay.selected_file > 0) vplay.selected_file--;
    else vplay.selected_file = vplay.file_count - 1;
    video_load_media(vplay.files[vplay.selected_file].path);
}

static inline uint8_t compute_dark_level(uint8_t g, uint8_t mode)
{
    if (mode == 2) {
        /* Sharp Pure B&W Threshold (No dither mesh/dots at all) */
        return (g < 128) ? 16 : 0;
    }
    if (mode == 0) {
        /* Clean High-Contrast Bayer: Solid blacks <= 30, Solid whites >= 225 */
        if (g <= 30) return 16;
        if (g >= 225) return 0;
        /* Remap 31..224 to 1..14 */
        return 1 + (uint8_t)(((uint32_t)(224 - g) * 14) / 194);
    }
    /* Mode 1: Standard linear Bayer (0..15) */
    return (uint8_t)((255U - g) >> 4);
}

/* Fast 4x4 Bayer dither from grayscale directly to 1-bit 400x300 XBM bitmap buffer */
static void video_convert_frame_to_xbm(const uint8_t *cur_gray, uint32_t src_w, uint32_t src_h, uint8_t *dst_xbm)
{
    static const uint8_t bayer4[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5}
    };

    const uint8_t d_mode = vplay.dither_mode;

    if (src_w == VIDEO_CANVAS_WIDTH && src_h == VIDEO_CANVAS_HEIGHT) {
        /* Direct 1:1 pixel mapping (ultra fast) */
        for (uint32_t y = 0; y < 300; y++) {
            uint8_t *line_dst = dst_xbm + (y * VIDEO_XBM_STRIDE);
            const uint8_t *line_src = cur_gray + (y * 400);
            const uint8_t *b_row = bayer4[y & 3];

            for (uint32_t bx = 0; bx < 50; bx++) {
                uint8_t val = 0;
                uint32_t x0 = bx * 8;
                for (uint32_t bit = 0; bit < 8; bit++) {
                    uint32_t x = x0 + bit;
                    uint8_t g = line_src[x];
                    uint8_t dark_level = compute_dark_level(g, d_mode);
                    if (b_row[x & 3] < dark_level) {
                        val |= (uint8_t)(1U << bit);
                    }
                }
                line_dst[bx] = val;
            }
        }
    } else {
        /* Scaled mapping with aspect ratio preservation */
        memset(dst_xbm, 0x00, VIDEO_XBM_BYTES); /* Default white canvas (0x00 = white in mono XBM) */

        int dw = (int)src_w;
        int dh = (int)src_h;
        float aspect = (float)dw / (float)dh;
        dw = VIDEO_CANVAS_WIDTH;
        dh = (int)((float)VIDEO_CANVAS_WIDTH / aspect);
        if (dh > (int)VIDEO_CANVAS_HEIGHT) {
            dh = VIDEO_CANVAS_HEIGHT;
            dw = (int)((float)VIDEO_CANVAS_HEIGHT * aspect);
        }

        const int ox = ((int)VIDEO_CANVAS_WIDTH - dw) / 2;
        const int oy = ((int)VIDEO_CANVAS_HEIGHT - dh) / 2;

        for (int dy = 0; dy < dh; dy++) {
            int target_y = oy + dy;
            if (target_y < 0 || target_y >= (int)VIDEO_CANVAS_HEIGHT) continue;

            uint32_t sy = (uint32_t)(((uint64_t)dy * src_h) / (uint32_t)dh);
            const uint8_t *src_row = cur_gray + (sy * src_w);
            uint8_t *dst_row = dst_xbm + (target_y * (int)VIDEO_XBM_STRIDE);
            const uint8_t *b_row = bayer4[target_y & 3];

            for (int dx = 0; dx < dw; dx++) {
                int target_x = ox + dx;
                if (target_x < 0 || target_x >= (int)VIDEO_CANVAS_WIDTH) continue;

                uint32_t sx = (uint32_t)(((uint64_t)dx * src_w) / (uint32_t)dw);
                uint8_t g = src_row[sx];
                uint8_t dark_level = compute_dark_level(g, d_mode);

                int bx = target_x / 8;
                int bit = target_x % 8;

                if (b_row[target_x & 3] < dark_level) {
                    dst_row[bx] |= (uint8_t)(1U << bit); /* Black pixel */
                }
            }
        }
    }
}

static void video_render_picker(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 16, "SOLAROS CINEMA VIDEO PLAYER");

    char count_txt[32];
    snprintf(count_txt, sizeof(count_txt), "%u videos", (unsigned)vplay.file_count);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t cw = solar_os_gfx_text_width(gfx, count_txt);
    solar_os_gfx_text(gfx, screen_w - (int)cw - 8, 16, count_txt);

    if (vplay.file_count == 0) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 30, 90, "No media files found on SD Card.");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 30, 115, "Formats supported:");
        solar_os_gfx_text(gfx, 40, 135, "- .slv   : SolarOS Ultra-Fast 2-bit Stream (20-50 FPS)");
        solar_os_gfx_text(gfx, 40, 155, "- .mjpeg : Motion JPEG Video (streaming from SD card)");
        solar_os_gfx_text(gfx, 40, 175, "- .gif   : Animated GIF (short clips <= 5s)");
        solar_os_gfx_text(gfx, 30, 215, "Use tools/solar_video_converter_gui.py on PC to convert any video!");
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
            if (vplay.files[idx].type == MEDIA_TYPE_SLV) type_lbl = "[SLV STREAM]";
            else if (vplay.files[idx].type == MEDIA_TYPE_MJPEG) type_lbl = vplay.files[idx].has_audio ? "[MJPEG + AUDIO]" : "[MJPEG VIDEO]";
            else if (vplay.files[idx].type == MEDIA_TYPE_GIF) type_lbl = "[GIF]";
            
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
}

static void video_render_playback(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || vplay.xbm_buf == NULL) return;

    const uint8_t *cur_gray = NULL;
    if (vplay.type == MEDIA_TYPE_GIF && vplay.gif_anim.gray != NULL) {
        const size_t frame_bytes = (size_t)vplay.width * vplay.height;
        cur_gray = vplay.gif_anim.gray + ((size_t)vplay.frame_index * frame_bytes);
    } else if ((vplay.type == MEDIA_TYPE_MJPEG || vplay.type == MEDIA_TYPE_SLV) && vplay.cur_frame_gray != NULL) {
        cur_gray = vplay.cur_frame_gray;
    }

    if (cur_gray == NULL || vplay.width == 0 || vplay.height == 0) return;

    /* 1. Ultra-Fast Dither to 1-bit XBM Frame Buffer (0.3 ms) */
    video_convert_frame_to_xbm(cur_gray, vplay.width, vplay.height, vplay.xbm_buf);

    /* 2. Direct DMA Hardware Present to ST7305 RLCD (5 ms) */
    solar_os_gfx_present_mono_xbm(gfx, vplay.xbm_buf, VIDEO_XBM_BYTES, 0, 0, 400, 300, VIDEO_XBM_STRIDE);
}

static void video_render(solar_os_context_t *ctx)
{
    if (vplay.in_picker) {
        video_render_picker(ctx);
    } else {
        video_render_playback(ctx);
    }
}

static uint32_t video_requested_tick_interval_ms(void)
{
    if (vplay.in_picker || !vplay.playing) {
        return 100U;
    }
    uint32_t d = (uint32_t)((float)vplay.frame_delay_ms / vplay.speed);
    if (d < 15) d = 15;
    return d;
}

static esp_err_t video_start(solar_os_context_t *ctx)
{
    video_log("=== Video Player Starting ===");
    vplay.speed = 1.0f;
    vplay.loop_mode = LOOP_MODE_ONE;
    vplay.playing = true;

    /* Allocate fast 15 KB XBM display buffer */
    vplay.xbm_buf = solar_os_memory_alloc(VIDEO_XBM_BYTES, SOLAR_OS_MEMORY_INTERNAL_PREFERRED, "video.xbm");
    if (vplay.xbm_buf == NULL) {
        vplay.xbm_buf = solar_os_memory_alloc(VIDEO_XBM_BYTES, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "video.xbm");
    }

    solar_os_context_set_graphics_active(ctx, true);

    const int argc = solar_os_context_argc(ctx);
    if (argc > 1) {
        const char *launch_path = solar_os_context_argv(ctx, 1);
        video_log("Video Player launched with argument: %s", launch_path ? launch_path : "NULL");
        if (launch_path != NULL && launch_path[0] != '\0') {
            esp_err_t err = video_load_media(launch_path);
            if (err == ESP_OK) {
                video_render(ctx);
                return ESP_OK;
            }
            video_log("Initial load failed (err=%s), opening file picker", esp_err_to_name(err));
        }
    }

    vplay.in_picker = true;
    video_refresh_file_list();
    video_render(ctx);
    return ESP_OK;
}

static void video_stop(solar_os_context_t *ctx)
{
    video_log("=== Video Player Stopping ===");
    solar_os_display_set_high_refresh_override("display0", false, 0);
    if (vplay.xbm_buf != NULL) {
        solar_os_memory_free(vplay.xbm_buf);
        vplay.xbm_buf = NULL;
    }
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
                if (vplay.type == MEDIA_TYPE_SLV && vplay.stream_file != NULL) {
                    if (video_read_next_slv_frame() == ESP_OK) {
                        uint32_t delay_ms = (uint32_t)((float)vplay.frame_delay_ms / vplay.speed);
                        if (delay_ms < 15) delay_ms = 15;
                        vplay.next_frame_us = now_us + (int64_t)delay_ms * 1000LL;
                        video_render(ctx);
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                } else if (vplay.type == MEDIA_TYPE_MJPEG && vplay.stream_file != NULL) {
                    if (video_read_next_mjpeg_frame() == ESP_OK) {
                        uint32_t delay_ms = (uint32_t)((float)vplay.frame_delay_ms / vplay.speed);
                        if (delay_ms < 15) delay_ms = 15;
                        vplay.next_frame_us = now_us + (int64_t)delay_ms * 1000LL;
                        video_render(ctx);
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                } else if (vplay.type == MEDIA_TYPE_GIF && vplay.gif_anim.gray != NULL && vplay.frame_count > 1) {
                    vplay.frame_index = (vplay.frame_index + 1) % vplay.frame_count;
                    uint32_t delay_ms = (vplay.gif_anim.delays_ms != NULL && vplay.gif_anim.delays_ms[vplay.frame_index] > 0) ?
                                        vplay.gif_anim.delays_ms[vplay.frame_index] : 66;
                    delay_ms = (uint32_t)((float)delay_ms / vplay.speed);
                    if (delay_ms < 15) delay_ms = 15;
                    vplay.next_frame_us = now_us + (int64_t)delay_ms * 1000LL;
                    video_render(ctx);
                    vTaskDelay(pdMS_TO_TICKS(1));
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
            /* Next / Previous Video Navigation */
            if (ch == 'n' || ch == 'N' || ch == '>' || ch == ']' || (uint8_t)ch == SOLAR_OS_KEY_PAGE_DOWN) {
                video_play_next();
                video_render(ctx);
                return true;
            }
            if (ch == 'p' || ch == 'P' || ch == '<' || ch == '[' || (uint8_t)ch == SOLAR_OS_KEY_PAGE_UP) {
                video_play_prev();
                video_render(ctx);
                return true;
            }

            /* Dither Mode Cycle: Clean Bayer (0) -> Standard Bayer (1) -> Pure Sharp B&W (2) */
            if (ch == 'm' || ch == 'M') {
                vplay.dither_mode = (vplay.dither_mode + 1) % 3;
                video_render(ctx);
                return true;
            }

            if (ch == ' ' || ch == '\r' || ch == '\n') {
                vplay.playing = !vplay.playing;
                video_render(ctx);
                return true;
            }
            if (ch == '\t' || ch == 'l' || ch == 'L') {
                /* Cycle loop modes: ONE -> ALL -> OFF */
                if (vplay.loop_mode == LOOP_MODE_ONE) vplay.loop_mode = LOOP_MODE_ALL;
                else if (vplay.loop_mode == LOOP_MODE_ALL) vplay.loop_mode = LOOP_MODE_OFF;
                else vplay.loop_mode = LOOP_MODE_ONE;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A') {
                if (vplay.type == MEDIA_TYPE_GIF && vplay.frame_count > 1) {
                    if (vplay.frame_index > 0) vplay.frame_index--;
                    else vplay.frame_index = vplay.frame_count - 1;
                }
                vplay.playing = false;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D') {
                if (vplay.type == MEDIA_TYPE_GIF && vplay.frame_count > 1) {
                    vplay.frame_index = (vplay.frame_index + 1) % vplay.frame_count;
                }
                vplay.playing = false;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
                if (vplay.speed < 3.0f) vplay.speed += 0.25f;
                video_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
                if (vplay.speed > 0.35f) vplay.speed -= 0.25f;
                video_render(ctx);
                return true;
            }
            if (ch == 'r' || ch == 'R') {
                vplay.frame_index = 0;
                if (vplay.stream_file != NULL) {
                    fseek(vplay.stream_file, (vplay.type == MEDIA_TYPE_SLV) ? (long)sizeof(slv_header_t) : 0, SEEK_SET);
                }
                vplay.next_frame_us = now_us;
                video_render(ctx);
                return true;
            }
            if (ch == 'o' || ch == 'O' || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
                solar_os_display_set_high_refresh_override("display0", false, 0);
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
    .tick_interval_ms = 15U,
    .requested_tick_interval_ms = video_requested_tick_interval_ms,
    .worker_stack_bytes = VIDEO_STACK_SIZE,
};
