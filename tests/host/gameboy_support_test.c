#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_gameboy_rom.h"
#include "solar_os_gameboy_video.h"

#define ENABLE_SOUND 0
#define PEANUT_GB_12_COLOUR 0
#include "vendor/peanut_gb/peanut_gb.h"

static uint8_t core_rom[32U * 1024U];
static size_t core_scanlines;

static uint8_t core_rom_read(struct gb_s *gb, uint_fast32_t address) {
  (void)gb;
  return address < sizeof(core_rom) ? core_rom[address] : 0xFFU;
}

static uint8_t core_ram_read(struct gb_s *gb, uint_fast32_t address) {
  (void)gb;
  (void)address;
  return 0xFFU;
}

static void core_ram_write(struct gb_s *gb, uint_fast32_t address,
                           uint8_t value) {
  (void)gb;
  (void)address;
  (void)value;
}

static void core_error(struct gb_s *gb, enum gb_error_e error,
                       uint16_t address) {
  (void)gb;
  fprintf(stderr, "Peanut-GB error %d at 0x%04x\n", (int)error,
          (unsigned)address);
  abort();
}

static void core_draw_line(struct gb_s *gb, const uint8_t *pixels,
                           uint_fast8_t line) {
  (void)gb;
  assert(pixels != NULL);
  assert(line < SOLAR_OS_GAMEBOY_LCD_HEIGHT);
  core_scanlines++;
}

static void finish_header(uint8_t *rom) {
  uint8_t checksum = 0;
  for (size_t i = 0x134U; i <= 0x14CU; i++) {
    checksum = (uint8_t)(checksum - rom[i] - 1U);
  }
  rom[0x14DU] = checksum;
}

static void test_rom_validation(void) {
  uint8_t *rom = calloc(1, 32U * 1024U);
  assert(rom != NULL);
  memcpy(&rom[0x134U], "SOLAROS TEST", 12U);
  rom[0x147U] = 0;
  rom[0x148U] = 0;
  rom[0x149U] = 0;
  finish_header(rom);

  assert(solar_os_gameboy_rom_validate(rom, 32U * 1024U) ==
         SOLAR_OS_GAMEBOY_ROM_OK);
  assert(solar_os_gameboy_rom_validate(rom, 0x14FU) ==
         SOLAR_OS_GAMEBOY_ROM_TOO_SMALL);
  rom[0x14DU] ^= 1U;
  assert(solar_os_gameboy_rom_validate(rom, 32U * 1024U) ==
         SOLAR_OS_GAMEBOY_ROM_BAD_CHECKSUM);
  rom[0x14DU] ^= 1U;
  rom[0x148U] = 1U;
  finish_header(rom);
  assert(solar_os_gameboy_rom_validate(rom, 32U * 1024U) ==
         SOLAR_OS_GAMEBOY_ROM_TRUNCATED);
  rom[0x148U] = 0U;
  rom[0x143U] = 0xC0U;
  finish_header(rom);
  assert(solar_os_gameboy_rom_validate(rom, 32U * 1024U) ==
         SOLAR_OS_GAMEBOY_ROM_CGB_ONLY);
  free(rom);
}

static size_t bitmap_popcount(const uint8_t *bitmap) {
  size_t count = 0;
  for (size_t i = 0; i < SOLAR_OS_GAMEBOY_BITMAP_BYTES; i++) {
    uint8_t value = bitmap[i];
    while (value != 0) {
      count += value & 1U;
      value >>= 1U;
    }
  }
  return count;
}

static size_t render_uniform_shade(uint8_t shade) {
  uint8_t bitmap[SOLAR_OS_GAMEBOY_BITMAP_BYTES];
  uint8_t line[SOLAR_OS_GAMEBOY_LCD_WIDTH];
  memset(line, shade, sizeof(line));
  solar_os_gameboy_video_clear(bitmap, sizeof(bitmap));
  for (size_t y = 0; y < SOLAR_OS_GAMEBOY_LCD_HEIGHT; y++) {
    assert(solar_os_gameboy_video_scanline(bitmap, sizeof(bitmap), line, y));
  }
  return bitmap_popcount(bitmap);
}

static void test_frame_dithering(void) {
  const size_t pixels =
      SOLAR_OS_GAMEBOY_BITMAP_WIDTH * SOLAR_OS_GAMEBOY_BITMAP_HEIGHT;
  assert(render_uniform_shade(0) == 0);
  assert(render_uniform_shade(1) == pixels * 5U / 16U);
  assert(render_uniform_shade(2) == pixels * 10U / 16U);
  assert(render_uniform_shade(3) == pixels);

  uint8_t bitmap[SOLAR_OS_GAMEBOY_BITMAP_BYTES] = {0};
  uint8_t line[SOLAR_OS_GAMEBOY_LCD_WIDTH] = {0};
  assert(!solar_os_gameboy_video_scanline(bitmap, sizeof(bitmap), line,
                                          SOLAR_OS_GAMEBOY_LCD_HEIGHT));
  assert(
      !solar_os_gameboy_video_scanline(bitmap, sizeof(bitmap) - 1U, line, 0));
}

static void test_vendored_core_frame(void) {
  memset(core_rom, 0, sizeof(core_rom));
  core_rom[0x100U] = 0xC3U; /* JP 0x0150, past the cartridge header. */
  core_rom[0x101U] = 0x50U;
  core_rom[0x102U] = 0x01U;
  core_rom[0x147U] = 0x00U; /* ROM-only cartridge. */
  core_rom[0x148U] = 0x00U; /* 32 KiB ROM. */
  core_rom[0x149U] = 0x00U; /* No cartridge RAM. */
  core_rom[0x150U] = 0xC3U; /* Infinite JP 0x0150 loop. */
  core_rom[0x151U] = 0x50U;
  core_rom[0x152U] = 0x01U;
  finish_header(core_rom);

  struct gb_s *core = calloc(1, sizeof(*core));
  assert(core != NULL);
  assert(gb_init(core, core_rom_read, core_ram_read, core_ram_write, core_error,
                 NULL) == GB_INIT_NO_ERROR);
  gb_init_lcd(core, core_draw_line);
  gb_run_frame(core); /* Complete the core's initial partial video frame. */
  core_scanlines = 0;
  gb_run_frame(core);
  assert(core_scanlines == SOLAR_OS_GAMEBOY_LCD_HEIGHT);
  free(core);
}

int main(void) {
  test_rom_validation();
  test_frame_dithering();
  test_vendored_core_frame();
  puts("gameboy support tests passed");
  return 0;
}
