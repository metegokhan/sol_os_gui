#include "solar_os_screenshot.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_err.h"
#include "solar_os_log.h"
#include "solar_os_storage.h"
#include "solar_os_terminal.h"

static const char *TAG = "screenshot";

/* External U8g2 rotation callbacks to test active rotation */
extern const u8g2_cb_t u8g2_cb_r0;
extern const u8g2_cb_t u8g2_cb_r1;
extern const u8g2_cb_t u8g2_cb_r2;
extern const u8g2_cb_t u8g2_cb_r3;

static uint8_t u8g2_extract_logical_pixel(u8g2_t *u8g2, uint16_t x, uint16_t y)
{
    if (u8g2 == NULL) {
        return 0;
    }
    uint8_t *buf = u8g2_GetBufferPtr(u8g2);
    if (buf == NULL) {
        return 0;
    }

    const u8x8_display_info_t *info = u8g2_GetU8x8(u8g2)->display_info;
    if (info == NULL) {
        return 0;
    }

    const uint8_t tile_width = info->tile_width;
    const uint16_t native_w = info->pixel_width;
    const uint16_t native_h = info->pixel_height;

    uint16_t nx = x;
    uint16_t ny = y;

    /* Transform logical (x, y) coordinates to native buffer (nx, ny) coordinates */
    if (u8g2->cb == &u8g2_cb_r1) {
        /* R1 (90 deg clockwise): logical_w = native_h, logical_h = native_w */
        if (y < native_w && x < native_h) {
            nx = (uint16_t)((native_w - 1) - y);
            ny = x;
        } else {
            return 0;
        }
    } else if (u8g2->cb == &u8g2_cb_r2) {
        /* R2 (180 deg) */
        if (x < native_w && y < native_h) {
            nx = (uint16_t)((native_w - 1) - x);
            ny = (uint16_t)((native_h - 1) - y);
        } else {
            return 0;
        }
    } else if (u8g2->cb == &u8g2_cb_r3) {
        /* R3 (270 deg clockwise) */
        if (y < native_h && x < native_w) {
            nx = y;
            ny = (uint16_t)((native_h - 1) - x);
        } else {
            return 0;
        }
    } else {
        /* R0 (0 deg) */
        if (x >= native_w || y >= native_h) {
            return 0;
        }
    }

    /* Vertical top LSB tile layout */
    const size_t byte_idx = (size_t)(ny / 8) * (tile_width * 8) + (size_t)(nx / 8) * 8 + (nx & 7);
    return (buf[byte_idx] >> (ny & 7)) & 1;
}

esp_err_t solar_os_screenshot_capture(u8g2_t *u8g2, char *saved_filename, size_t filename_size)
{
    if (u8g2 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *buf = u8g2_GetBufferPtr(u8g2);
    if (buf == NULL) {
        SOLAR_OS_LOGE(TAG, "Framebuffer buffer is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t width = u8g2_GetDisplayWidth(u8g2);
    const uint16_t height = u8g2_GetDisplayHeight(u8g2);

    if (width == 0 || height == 0) {
        SOLAR_OS_LOGE(TAG, "Invalid display dimensions: %ux%u", width, height);
        return ESP_ERR_INVALID_STATE;
    }

    /* Target directory */
    const char *dir = "/sdcard/screencapture";
    struct stat st;
    if (stat(dir, &st) != 0) {
        if (mkdir(dir, 0777) != 0) {
            SOLAR_OS_LOGW(TAG, "Failed to create directory %s", dir);
        }
    }

    /* Generate unique filename */
    char filepath[128] = {0};
    static uint32_t capture_index = 1;
    
    for (uint32_t i = capture_index; i < 9999; i++) {
        snprintf(filepath, sizeof(filepath), "%s/snap_%04u.bmp", dir, (unsigned)i);
        if (stat(filepath, &st) != 0) {
            capture_index = i + 1;
            break;
        }
    }

    FILE *f = fopen(filepath, "wb");
    if (f == NULL) {
        /* If /sdcard failed, try fallback /screencapture */
        snprintf(filepath, sizeof(filepath), "/screencapture/snap_%04u.bmp", (unsigned)capture_index);
        mkdir("/screencapture", 0777);
        f = fopen(filepath, "wb");
    }

    if (f == NULL) {
        SOLAR_OS_LOGE(TAG, "Failed to create screenshot file (SD card not mounted?)");
        return ESP_FAIL;
    }

    /* 1-bit monochrome BMP calculations */
    const uint32_t row_size = ((width + 31) / 32) * 4; /* 32-bit aligned */
    const uint32_t image_size = row_size * height;
    const uint32_t file_size = 14 + 40 + 8 + image_size;

    /* BMP File Header (14 bytes) */
    const uint8_t file_hdr[14] = {
        'B', 'M',
        (uint8_t)(file_size), (uint8_t)(file_size >> 8), (uint8_t)(file_size >> 16), (uint8_t)(file_size >> 24),
        0, 0, 0, 0,
        62, 0, 0, 0 /* offset to pixel data: 14 + 40 + 8 = 62 */
    };
    if (fwrite(file_hdr, 1, 14, f) != 14) {
        fclose(f);
        return ESP_FAIL;
    }

    /* BMP Info Header (40 bytes) */
    const uint8_t info_hdr[40] = {
        40, 0, 0, 0, /* header size */
        (uint8_t)(width), (uint8_t)(width >> 8), 0, 0,
        (uint8_t)(height), (uint8_t)(height >> 8), 0, 0,
        1, 0, /* planes */
        1, 0, /* 1 bit per pixel */
        0, 0, 0, 0, /* BI_RGB (uncompressed) */
        (uint8_t)(image_size), (uint8_t)(image_size >> 8), (uint8_t)(image_size >> 16), (uint8_t)(image_size >> 24),
        0x13, 0x0B, 0, 0, /* 2835 ppm (72 DPI) */
        0x13, 0x0B, 0, 0,
        2, 0, 0, 0, /* 2 colors used */
        2, 0, 0, 0
    };
    if (fwrite(info_hdr, 1, 40, f) != 40) {
        fclose(f);
        return ESP_FAIL;
    }

    /* In u8g2 ST7305 RLCD buffer: Bit 1 is White (background), Bit 0 is Black (text/lines) */
    const bool sys_inverted = solar_os_terminal_palette_preference_inverted();
    uint8_t palette[8];
    if (!sys_inverted) {
        /* Standard Light theme: Bit 0 = Black (text/lines), Bit 1 = White (background) */
        palette[0] = 0x00; palette[1] = 0x00; palette[2] = 0x00; palette[3] = 0x00; /* Color 0: Black */
        palette[4] = 0xFF; palette[5] = 0xFF; palette[6] = 0xFF; palette[7] = 0x00; /* Color 1: White */
    } else {
        /* Inverted Dark theme: Bit 0 = White (text/lines), Bit 1 = Black (background) */
        palette[0] = 0xFF; palette[1] = 0xFF; palette[2] = 0xFF; palette[3] = 0x00; /* Color 0: White */
        palette[4] = 0x00; palette[5] = 0x00; palette[6] = 0x00; palette[7] = 0x00; /* Color 1: Black */
    }
    if (fwrite(palette, 1, 8, f) != 8) {
        fclose(f);
        return ESP_FAIL;
    }

    /* Write Rows (bottom-up: row height-1 down to 0) */
    uint8_t *row_buf = (uint8_t *)malloc(row_size);
    if (row_buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    for (int y = (int)height - 1; y >= 0; y--) {
        memset(row_buf, 0, row_size);
        for (uint16_t x = 0; x < width; x++) {
            if (u8g2_extract_logical_pixel(u8g2, x, (uint16_t)y)) {
                row_buf[x / 8] |= (uint8_t)(1 << (7 - (x % 8)));
            }
        }
        if (fwrite(row_buf, 1, row_size, f) != row_size) {
            free(row_buf);
            fclose(f);
            return ESP_FAIL;
        }
    }

    free(row_buf);
    fclose(f);
    SOLAR_OS_LOGI(TAG, "Captured %ux%u screenshot to %s (%u bytes)", width, height, filepath, (unsigned)file_size);

    if (saved_filename != NULL && filename_size > 0) {
        const char *slash = strrchr(filepath, '/');
        strlcpy(saved_filename, slash ? slash + 1 : filepath, filename_size);
    }

    return ESP_OK;
}
