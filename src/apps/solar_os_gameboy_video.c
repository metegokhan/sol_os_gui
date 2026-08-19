#include "solar_os_gameboy_video.h"

#include <string.h>

static const uint8_t bayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static inline void gameboy_bitmap_set(uint8_t *bitmap, size_t x, size_t y) {
  const size_t offset = y * SOLAR_OS_GAMEBOY_BITMAP_STRIDE + (x >> 3U);
  bitmap[offset] |= (uint8_t)(1U << (x & 7U));
}

void solar_os_gameboy_video_clear(uint8_t *bitmap, size_t bitmap_len) {
  if (bitmap == NULL || bitmap_len < SOLAR_OS_GAMEBOY_BITMAP_BYTES) {
    return;
  }
  memset(bitmap, 0, SOLAR_OS_GAMEBOY_BITMAP_BYTES);
}

bool solar_os_gameboy_video_scanline(uint8_t *bitmap, size_t bitmap_len,
                                     const uint8_t *pixels, size_t line) {
  if (bitmap == NULL || pixels == NULL ||
      bitmap_len < SOLAR_OS_GAMEBOY_BITMAP_BYTES ||
      line >= SOLAR_OS_GAMEBOY_LCD_HEIGHT) {
    return false;
  }

  const size_t output_y = line * SOLAR_OS_GAMEBOY_SCALE;
  memset(bitmap + output_y * SOLAR_OS_GAMEBOY_BITMAP_STRIDE, 0,
         SOLAR_OS_GAMEBOY_BITMAP_STRIDE * SOLAR_OS_GAMEBOY_SCALE);

  static const uint8_t shade_cutoff[4] = {0, 5, 10, 16};
  for (size_t source_x = 0; source_x < SOLAR_OS_GAMEBOY_LCD_WIDTH; source_x++) {
    const uint8_t shade = pixels[source_x] & 0x03U;
    const uint8_t cutoff = shade_cutoff[shade];
    const size_t output_x = source_x * SOLAR_OS_GAMEBOY_SCALE;
    for (size_t dy = 0; dy < SOLAR_OS_GAMEBOY_SCALE; dy++) {
      for (size_t dx = 0; dx < SOLAR_OS_GAMEBOY_SCALE; dx++) {
        const size_t x = output_x + dx;
        const size_t y = output_y + dy;
        if (bayer4[y & 3U][x & 3U] < cutoff) {
          gameboy_bitmap_set(bitmap, x, y);
        }
      }
    }
  }
  return true;
}
