#include "solar_os_screenshot.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_err.h"
#include "solar_os_log.h"
#include "solar_os_storage.h"

static const char *TAG = "screenshot";

static uint8_t u8x8_get_pixel_1(uint16_t x, uint16_t y, uint8_t *dest_ptr, uint8_t tile_width)
{
    dest_ptr += (y / 8) * tile_width * 8;
    y &= 7;
    dest_ptr += x;
    if ((*dest_ptr & (1 << y)) == 0) {
        return 0;
    }
    return 1;
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
    const uint8_t tile_width = u8g2_GetBufferTileWidth(u8g2);

    if (width == 0 || height == 0 || tile_width == 0) {
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

    /* Color Palette (8 bytes: Palette 0 = White, Palette 1 = Black) */
    const uint8_t palette[8] = {
        0xFF, 0xFF, 0xFF, 0x00, /* Color 0: White */
        0x00, 0x00, 0x00, 0x00  /* Color 1: Black */
    };
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
            if (u8x8_get_pixel_1(x, (uint16_t)y, buf, tile_width)) {
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
