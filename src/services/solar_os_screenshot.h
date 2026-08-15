#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Captures the current u8g2 framebuffer and writes a 1-bit monochrome BMP to /sdcard/screencapture/
 * 
 * @param u8g2 Pointer to the active u8g2 display instance
 * @param saved_filename Buffer to receive the saved filename (e.g. "snap_0001.bmp")
 * @param filename_size Size of saved_filename buffer
 * @return ESP_OK on success, ESP_FAIL on file/directory error
 */
esp_err_t solar_os_screenshot_capture(u8g2_t *u8g2, char *saved_filename, size_t filename_size);

#ifdef __cplusplus
}
#endif
