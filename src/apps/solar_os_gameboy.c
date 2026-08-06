#include "solar_os_gameboy.h"

#include <inttypes.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_timer.h"
#include "solar_os_config.h"
#include "solar_os_gameboy_audio.h"
#include "solar_os_gameboy_presenter.h"
#include "solar_os_gameboy_rom.h"
#include "solar_os_gameboy_video.h"
#include "solar_os_gfx.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

#if SOLAR_OS_PACKAGE_SERVICE_SYNTH
#define audio_read solar_os_gameboy_audio_read
#define audio_write solar_os_gameboy_audio_write
#define ENABLE_SOUND 1
#else
#define ENABLE_SOUND 0
#endif
#define PEANUT_GB_12_COLOUR 0
#include "vendor/peanut_gb/peanut_gb.h"
#if SOLAR_OS_PACKAGE_SERVICE_SYNTH
#undef audio_read
#undef audio_write
#endif

#define GAMEBOY_FRAME_PERIOD_US 16743LL
#define GAMEBOY_INPUT_PULSE_US 140000LL
#define GAMEBOY_STATS_FRAMES 120U
#define GAMEBOY_ROM_BANK_BYTES (16U * 1024U)
#define GAMEBOY_PRESENT_DIVISOR 3U

typedef struct {
  struct gb_s *core;
  uint8_t *rom;
  size_t rom_size;
  uint8_t *rom_fixed_cache;
  uint8_t *rom_switch_cache;
  size_t rom_switch_cache_base;
  uint8_t *cart_ram;
  size_t cart_ram_size;
  uint8_t *bitmap;
  char rom_path[SOLAR_OS_STORAGE_PATH_MAX];
  char save_path[SOLAR_OS_STORAGE_PATH_MAX];
  char title[17];
  int64_t release_at[8];
  int64_t next_frame_us;
  int64_t stats_started_us;
  uint64_t emulation_us;
  uint32_t stats_emulated_frames;
  uint32_t emulated_since_present;
  enum gb_error_e core_error;
  uint16_t core_error_address;
  jmp_buf core_error_jump;
  bool core_error_jump_ready;
  bool loaded;
  bool suspended;
  bool paused;
  bool cart_ram_dirty;
  bool frame_rendered;
} gameboy_state_t;

static const char *TAG = "solar_os_gameboy";
static gameboy_state_t gameboy;

static bool gameboy_make_save_path(const char *rom_path, char *save_path,
                                   size_t save_path_len) {
  if (rom_path == NULL || save_path == NULL || save_path_len == 0) {
    return false;
  }
  const char *slash = strrchr(rom_path, '/');
  const char *dot = strrchr(rom_path, '.');
  size_t base_len = strlen(rom_path);
  if (dot != NULL && (slash == NULL || dot > slash)) {
    base_len = (size_t)(dot - rom_path);
  }
  if (base_len + sizeof(".sav") > save_path_len) {
    save_path[0] = '\0';
    return false;
  }
  memcpy(save_path, rom_path, base_len);
  memcpy(save_path + base_len, ".sav", sizeof(".sav"));
  return true;
}

static esp_err_t gameboy_write_save(void) {
  if (!gameboy.cart_ram_dirty || gameboy.cart_ram == NULL ||
      gameboy.cart_ram_size == 0 || gameboy.save_path[0] == '\0') {
    return ESP_OK;
  }

  char temporary[SOLAR_OS_STORAGE_PATH_MAX + 5U];
  if (snprintf(temporary, sizeof(temporary), "%s.tmp", gameboy.save_path) >=
      (int)sizeof(temporary)) {
    return ESP_ERR_INVALID_SIZE;
  }
  FILE *file = fopen(temporary, "wb");
  if (file == NULL) {
    return ESP_FAIL;
  }
  const bool failed = fwrite(gameboy.cart_ram, 1, gameboy.cart_ram_size,
                             file) != gameboy.cart_ram_size ||
                      fflush(file) != 0 || fsync(fileno(file)) != 0;
  if (fclose(file) != 0 || failed) {
    (void)remove(temporary);
    return ESP_FAIL;
  }
  if (rename(temporary, gameboy.save_path) != 0) {
    (void)remove(temporary);
    return ESP_FAIL;
  }
  gameboy.cart_ram_dirty = false;
  return ESP_OK;
}

static void gameboy_free_state(bool save) {
  solar_os_gameboy_presenter_deinit();
  solar_os_gameboy_audio_deinit();
  if (save) {
    const esp_err_t err = gameboy_write_save();
    if (err != ESP_OK) {
      SOLAR_OS_LOGW(TAG, "save write failed: %s", esp_err_to_name(err));
    }
  }
  solar_os_memory_free(gameboy.bitmap);
  solar_os_memory_free(gameboy.cart_ram);
  solar_os_memory_free(gameboy.rom_switch_cache);
  solar_os_memory_free(gameboy.rom_fixed_cache);
  solar_os_memory_free(gameboy.core);
  solar_os_memory_free(gameboy.rom);
  memset(&gameboy, 0, sizeof(gameboy));
}

static esp_err_t gameboy_start_error(solar_os_context_t *ctx, const char *path,
                                     const char *detail) {
  char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
  snprintf(message, sizeof(message), "gameboy: %s%s%s",
           path != NULL ? path : "start failed",
           detail != NULL ? ": " : "", detail != NULL ? detail : "");
  SOLAR_OS_LOGE(TAG, "start failed: %s%s%s",
                path != NULL ? path : "gameboy",
                detail != NULL ? ": " : "",
                detail != NULL ? detail : "unknown error");
  solar_os_context_set_status_message(ctx, message);
  gameboy_free_state(false);
  solar_os_context_set_graphics_active(ctx, false);
  solar_os_context_request_exit(ctx);
  return ESP_OK;
}

static uint8_t gameboy_rom_read(struct gb_s *core,
                                const uint_fast32_t address) {
  gameboy_state_t *state = core->direct.priv;
  if (state == NULL || state->rom == NULL || address >= state->rom_size) {
    return 0xFFU;
  }

  if (address < GAMEBOY_ROM_BANK_BYTES && state->rom_fixed_cache != NULL) {
    return state->rom_fixed_cache[address];
  }
  if (address >= GAMEBOY_ROM_BANK_BYTES && state->rom_switch_cache != NULL) {
    const size_t bank_base = (size_t)address & ~(GAMEBOY_ROM_BANK_BYTES - 1U);
    if (bank_base != state->rom_switch_cache_base) {
      const size_t available = state->rom_size - bank_base;
      const size_t copy_len = available < GAMEBOY_ROM_BANK_BYTES
                                  ? available
                                  : GAMEBOY_ROM_BANK_BYTES;
      memcpy(state->rom_switch_cache, state->rom + bank_base, copy_len);
      if (copy_len < GAMEBOY_ROM_BANK_BYTES) {
        memset(state->rom_switch_cache + copy_len, 0xFF,
               GAMEBOY_ROM_BANK_BYTES - copy_len);
      }
      state->rom_switch_cache_base = bank_base;
    }
    return state
        ->rom_switch_cache[(size_t)address - state->rom_switch_cache_base];
  }
  return state->rom[address];
}

static uint8_t gameboy_cart_ram_read(struct gb_s *core,
                                     const uint_fast32_t address) {
  gameboy_state_t *state = core->direct.priv;
  if (state == NULL || state->cart_ram == NULL ||
      address >= state->cart_ram_size) {
    return 0xFFU;
  }
  return state->cart_ram[address];
}

static void gameboy_cart_ram_write(struct gb_s *core,
                                   const uint_fast32_t address,
                                   const uint8_t value) {
  gameboy_state_t *state = core->direct.priv;
  if (state == NULL || state->cart_ram == NULL ||
      address >= state->cart_ram_size) {
    return;
  }
  if (state->cart_ram[address] != value) {
    state->cart_ram[address] = value;
    state->cart_ram_dirty = true;
  }
}

static void gameboy_core_error(struct gb_s *core, const enum gb_error_e error,
                               const uint16_t address) {
  gameboy_state_t *state = core->direct.priv;
  if (state != NULL) {
    state->core_error = error;
    state->core_error_address = address;
    if (state->core_error_jump_ready) {
      longjmp(state->core_error_jump, 1);
    }
  }
  abort();
}

static void gameboy_draw_line(struct gb_s *core, const uint8_t *pixels,
                              const uint_fast8_t line) {
  gameboy_state_t *state = core->direct.priv;
  if (state == NULL || state->bitmap == NULL) {
    return;
  }
  state->frame_rendered = true;
  (void)solar_os_gameboy_video_scanline(
      state->bitmap, SOLAR_OS_GAMEBOY_BITMAP_BYTES, pixels, (size_t)line);
}

static uint8_t *gameboy_alloc_internal_cache(const char *tag) {
  uint8_t *cache = solar_os_memory_alloc(
      GAMEBOY_ROM_BANK_BYTES, SOLAR_OS_MEMORY_INTERNAL_PREFERRED, tag);
  if (cache != NULL && solar_os_memory_is_external(cache)) {
    solar_os_memory_free(cache);
    return NULL;
  }
  return cache;
}

static void gameboy_prepare_rom_caches(void) {
  gameboy.rom_switch_cache_base = SIZE_MAX;
  gameboy.rom_fixed_cache = gameboy_alloc_internal_cache("gameboy.rom0");
  if (gameboy.rom_fixed_cache != NULL) {
    memcpy(gameboy.rom_fixed_cache, gameboy.rom, GAMEBOY_ROM_BANK_BYTES);
  }
  gameboy.rom_switch_cache = gameboy_alloc_internal_cache("gameboy.rombank");
}

static bool gameboy_load_rom(const char *path, const struct stat *st) {
  if (path == NULL || st == NULL || st->st_size <= 0 ||
      (uint64_t)st->st_size > SOLAR_OS_GAMEBOY_ROM_MAX_BYTES) {
    return false;
  }
  gameboy.rom_size = (size_t)st->st_size;
  gameboy.rom = solar_os_memory_alloc(
      gameboy.rom_size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "gameboy.rom");
  if (gameboy.rom == NULL) {
    return false;
  }
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return false;
  }
  const size_t read_len = fread(gameboy.rom, 1, gameboy.rom_size, file);
  const bool close_failed = fclose(file) != 0;
  return read_len == gameboy.rom_size && !close_failed;
}

static void gameboy_load_save(void) {
  if (gameboy.cart_ram == NULL || gameboy.cart_ram_size == 0) {
    return;
  }
  memset(gameboy.cart_ram, 0xFF, gameboy.cart_ram_size);
  if (gameboy.save_path[0] == '\0') {
    return;
  }
  FILE *file = fopen(gameboy.save_path, "rb");
  if (file == NULL) {
    return;
  }
  const size_t read_len =
      fread(gameboy.cart_ram, 1, gameboy.cart_ram_size, file);
  if (read_len != gameboy.cart_ram_size) {
    SOLAR_OS_LOGW(TAG, "save size mismatch: read=%u expected=%u",
                  (unsigned)read_len, (unsigned)gameboy.cart_ram_size);
  }
  (void)fclose(file);
}

static void gameboy_render(void) {
  (void)solar_os_gameboy_presenter_queue(gameboy.bitmap);
}

static void gameboy_log_stats(int64_t now_us) {
  if (gameboy.stats_emulated_frames < GAMEBOY_STATS_FRAMES) {
    return;
  }
  const int64_t elapsed_us = now_us - gameboy.stats_started_us;
  const uint64_t emu_fps_x100 = elapsed_us > 0
                                    ? (uint64_t)gameboy.stats_emulated_frames *
                                          100000000ULL / (uint64_t)elapsed_us
                                    : 0;
  solar_os_gameboy_presenter_stats_t present;
  solar_os_gameboy_presenter_take_stats(&present);
  const uint64_t present_fps_x100 =
      elapsed_us > 0 ? (uint64_t)present.presented_frames * 100000000ULL /
                           (uint64_t)elapsed_us
                     : 0;
  SOLAR_OS_LOGI(
      TAG,
      "frames=%u emu_fps=%" PRIu64 ".%02" PRIu64 " present_fps=%" PRIu64
      ".%02" PRIu64 " avg_emu_us=%" PRIu64 " avg_present_us=%" PRIu64
      " dropped=%u",
      (unsigned)gameboy.stats_emulated_frames, emu_fps_x100 / 100U,
      emu_fps_x100 % 100U, present_fps_x100 / 100U, present_fps_x100 % 100U,
      gameboy.emulation_us / gameboy.stats_emulated_frames,
      present.presented_frames > 0
          ? present.present_us / present.presented_frames
          : 0,
      (unsigned)present.dropped_frames);
  gameboy.stats_emulated_frames = 0;
  gameboy.emulation_us = 0;
  gameboy.stats_started_us = now_us;
}

static esp_err_t gameboy_start(solar_os_context_t *ctx) {
  memset(&gameboy, 0, sizeof(gameboy));
  const char *path_arg = solar_os_context_argv(ctx, 1);
  if (path_arg == NULL) {
    return gameboy_start_error(ctx, NULL, "usage: gameboy <file.gb>");
  }
#if !SOLAR_OS_BOARD_WAVESHARE_ESP32_S3_RLCD_4_2 &&                       \
    !SOLAR_OS_BOARD_FREENOVE_ESP32_WROVER_V3
  return gameboy_start_error(ctx, path_arg,
                             "unsupported display target");
#endif

  esp_err_t err = solar_os_storage_resolve_path(path_arg, gameboy.rom_path,
                                                sizeof(gameboy.rom_path));
  if (err != ESP_OK) {
    return gameboy_start_error(ctx, path_arg, esp_err_to_name(err));
  }
  struct stat st;
  if (stat(gameboy.rom_path, &st) != 0 || !S_ISREG(st.st_mode)) {
    return gameboy_start_error(ctx, gameboy.rom_path, "file not found");
  }
  if (!gameboy_load_rom(gameboy.rom_path, &st)) {
    return gameboy_start_error(ctx, gameboy.rom_path,
                               "read or PSRAM allocation failed");
  }
  const solar_os_gameboy_rom_status_t rom_status =
      solar_os_gameboy_rom_validate(gameboy.rom, gameboy.rom_size);
  if (rom_status != SOLAR_OS_GAMEBOY_ROM_OK) {
    return gameboy_start_error(ctx, gameboy.rom_path,
                               solar_os_gameboy_rom_status_name(rom_status));
  }

  solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
  if (gfx == NULL || solar_os_gfx_width(gfx) < SOLAR_OS_GAMEBOY_BITMAP_WIDTH ||
      solar_os_gfx_height(gfx) < SOLAR_OS_GAMEBOY_BITMAP_HEIGHT) {
    return gameboy_start_error(ctx, gameboy.rom_path,
                               "display must be at least 320x288");
  }
  gameboy.core = solar_os_memory_calloc(1, sizeof(*gameboy.core),
                                        SOLAR_OS_MEMORY_INTERNAL_PREFERRED,
                                        "gameboy.core");
  gameboy.bitmap = solar_os_memory_calloc(1, SOLAR_OS_GAMEBOY_BITMAP_BYTES,
                                          SOLAR_OS_MEMORY_INTERNAL_PREFERRED,
                                          "gameboy.frame");
  if (gameboy.core == NULL || gameboy.bitmap == NULL) {
    return gameboy_start_error(ctx, gameboy.rom_path,
                               "emulator state allocation failed");
  }
  gameboy_prepare_rom_caches();

  const enum gb_init_error_e init_error =
      gb_init(gameboy.core, gameboy_rom_read, gameboy_cart_ram_read,
              gameboy_cart_ram_write, gameboy_core_error, &gameboy);
  if (init_error != GB_INIT_NO_ERROR) {
    const char *detail = init_error == GB_INIT_CARTRIDGE_UNSUPPORTED
                             ? "unsupported cartridge controller"
                             : "core rejected cartridge header";
    return gameboy_start_error(ctx, gameboy.rom_path, detail);
  }
  if (gb_get_save_size_s(gameboy.core, &gameboy.cart_ram_size) != 0) {
    return gameboy_start_error(ctx, gameboy.rom_path,
                               "invalid cartridge RAM size");
  }
  if (gameboy.cart_ram_size > 0) {
    gameboy.cart_ram = solar_os_memory_alloc(gameboy.cart_ram_size,
                                             SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                             "gameboy.save");
    if (gameboy.cart_ram == NULL) {
      return gameboy_start_error(ctx, gameboy.rom_path,
                                 "save RAM allocation failed");
    }
    (void)gameboy_make_save_path(gameboy.rom_path, gameboy.save_path,
                                 sizeof(gameboy.save_path));
    gameboy_load_save();
  }

  gb_init_lcd(gameboy.core, gameboy_draw_line);
  err = solar_os_gameboy_presenter_init(gfx);
  if (err != ESP_OK) {
    return gameboy_start_error(ctx, gameboy.rom_path,
                               "display presenter failed");
  }
  const esp_err_t audio_err = solar_os_gameboy_audio_init();
  if (audio_err != ESP_OK) {
    SOLAR_OS_LOGW(TAG, "starting without audio: %s",
                  esp_err_to_name(audio_err));
  }
  (void)gb_get_rom_name(gameboy.core, gameboy.title);
  time_t now_seconds = time(NULL);
  struct tm local_time;
  if (now_seconds > 0 && localtime_r(&now_seconds, &local_time) != NULL) {
    gb_set_rtc(gameboy.core, &local_time);
  }

  gameboy.loaded = true;
  gameboy.core->direct.joypad = 0xFFU;
  gameboy.core->direct.frame_skip = false;
  solar_os_context_set_graphics_active(ctx, true);
  const int64_t now_us = esp_timer_get_time();
  gameboy.next_frame_us = now_us;
  gameboy.stats_started_us = now_us;
  const unsigned cache_kib =
      (gameboy.rom_fixed_cache != NULL ? GAMEBOY_ROM_BANK_BYTES : 0U) / 1024U +
      (gameboy.rom_switch_cache != NULL ? GAMEBOY_ROM_BANK_BYTES : 0U) / 1024U;
  SOLAR_OS_LOGI(TAG,
                "loaded %s title=%.16s rom=%u save=%u core=%s frame=%s "
                "rom_cache=%uKiB present=1/%u",
                gameboy.rom_path, gameboy.title, (unsigned)gameboy.rom_size,
                (unsigned)gameboy.cart_ram_size,
                solar_os_memory_is_external(gameboy.core) ? "PSRAM" : "SRAM",
                solar_os_memory_is_external(gameboy.bitmap) ? "PSRAM" : "SRAM",
                cache_kib, (unsigned)GAMEBOY_PRESENT_DIVISOR);
  gameboy_render();
  return ESP_OK;
}

static void gameboy_stop(solar_os_context_t *ctx) {
  gameboy_free_state(true);
  solar_os_context_set_graphics_active(ctx, false);
}

static void gameboy_suspend(solar_os_context_t *ctx) {
  gameboy.suspended = true;
  solar_os_gameboy_presenter_suspend();
  solar_os_gameboy_audio_suspend();
  if (gameboy.core != NULL) {
    gameboy.core->direct.joypad = 0xFFU;
  }
  const esp_err_t err = gameboy_write_save();
  if (err != ESP_OK) {
    SOLAR_OS_LOGW(TAG, "save on suspend failed: %s", esp_err_to_name(err));
  }
  solar_os_context_set_graphics_active(ctx, false);
}

static void gameboy_resume(solar_os_context_t *ctx) {
  gameboy.suspended = false;
  const esp_err_t presenter_err = solar_os_gameboy_presenter_resume();
  if (presenter_err != ESP_OK) {
    SOLAR_OS_LOGW(TAG, "presenter resume failed: %s",
                  esp_err_to_name(presenter_err));
  }
  const esp_err_t audio_err = solar_os_gameboy_audio_resume();
  if (audio_err != ESP_OK) {
    SOLAR_OS_LOGW(TAG, "audio resume failed: %s", esp_err_to_name(audio_err));
  }
  const int64_t now_us = esp_timer_get_time();
  gameboy.next_frame_us = now_us;
  gameboy.stats_started_us = now_us;
  gameboy.stats_emulated_frames = 0;
  gameboy.emulation_us = 0;
  solar_os_gameboy_presenter_stats_t discarded;
  solar_os_gameboy_presenter_take_stats(&discarded);
  solar_os_context_set_graphics_active(ctx, true);
  gameboy_render();
}

static uint8_t gameboy_button_for_char(uint8_t ch) {
  if (ch == SOLAR_OS_KEY_RIGHT || ch == SOLAR_OS_KEY_CTRL_RIGHT) {
    return JOYPAD_RIGHT;
  }
  if (ch == SOLAR_OS_KEY_LEFT || ch == SOLAR_OS_KEY_CTRL_LEFT) {
    return JOYPAD_LEFT;
  }
  if (ch == SOLAR_OS_KEY_UP || ch == SOLAR_OS_KEY_CTRL_UP) {
    return JOYPAD_UP;
  }
  if (ch == SOLAR_OS_KEY_DOWN || ch == SOLAR_OS_KEY_CTRL_DOWN) {
    return JOYPAD_DOWN;
  }
  if (ch == 'z' || ch == 'Z') {
    return JOYPAD_A;
  }
  if (ch == 'x' || ch == 'X') {
    return JOYPAD_B;
  }
  if (ch == '\n' || ch == '\r') {
    return JOYPAD_START;
  }
  if (ch == '\b' || ch == SOLAR_OS_KEY_DELETE) {
    return JOYPAD_SELECT;
  }
  return 0;
}

static uint8_t gameboy_button_for_input_key(
    const solar_os_input_key_event_t *key) {
  if (key == NULL) {
    return 0;
  }
  /* Keep game controls at fixed physical positions across keyboard layouts. */
  if (key->usage == 0x1dU) {
    return JOYPAD_A;
  }
  if (key->usage == 0x1bU) {
    return JOYPAD_B;
  }
  if (key->usage >= 0x04U && key->usage <= 0x1dU) {
    return 0;
  }
  return gameboy_button_for_char(key->key);
}

static uint8_t gameboy_input_pressed_mask(void) {
  uint8_t pressed = 0;
  solar_os_input_key_event_t keys[SOLAR_OS_INPUT_MAX_PRESSED_KEYS];
  const size_t count = solar_os_input_get_pressed(
      keys, sizeof(keys) / sizeof(keys[0]));
  for (size_t i = 0; i < count; i++) {
    pressed |= gameboy_button_for_input_key(&keys[i]);
  }
  return pressed;
}

static uint8_t gameboy_pulse_pressed_mask(int64_t now_us) {
  uint8_t pressed = 0;
  if (gameboy.core == NULL) {
    return 0;
  }
  for (size_t bit = 0; bit < 8U; bit++) {
    if (gameboy.release_at[bit] != 0 && now_us >= gameboy.release_at[bit]) {
      gameboy.release_at[bit] = 0;
    }
    if (gameboy.release_at[bit] != 0) {
      pressed |= (uint8_t)(1U << bit);
    }
  }
  return pressed;
}

static void gameboy_refresh_inputs(int64_t now_us) {
  if (gameboy.core == NULL) {
    return;
  }
  const uint8_t pressed =
      gameboy_pulse_pressed_mask(now_us) | gameboy_input_pressed_mask();
  gameboy.core->direct.joypad = (uint8_t)~pressed;
}

static void gameboy_press(uint8_t mask, int64_t now_us) {
  if (gameboy.core == NULL) {
    return;
  }
  for (size_t bit = 0; bit < 8U; bit++) {
    if ((mask & (uint8_t)(1U << bit)) != 0) {
      gameboy.release_at[bit] = now_us + GAMEBOY_INPUT_PULSE_US;
    }
  }
  gameboy_refresh_inputs(now_us);
}

static bool gameboy_handle_char(solar_os_context_t *ctx, uint8_t ch) {
  if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q') {
    solar_os_context_request_exit(ctx);
    return true;
  }
  if (ch == 'p') {
    gameboy.paused = !gameboy.paused;
    if (gameboy.paused) {
      solar_os_gameboy_audio_suspend();
    } else {
      const esp_err_t audio_err = solar_os_gameboy_audio_resume();
      if (audio_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "audio resume failed: %s",
                      esp_err_to_name(audio_err));
      }
    }
    gameboy.core->direct.joypad = 0xFFU;
    memset(gameboy.release_at, 0, sizeof(gameboy.release_at));
    gameboy.next_frame_us = esp_timer_get_time();
    return true;
  }
  if (ch == 'r') {
    gb_reset(gameboy.core);
    solar_os_gameboy_audio_reset();
    solar_os_gameboy_video_clear(gameboy.bitmap, SOLAR_OS_GAMEBOY_BITMAP_BYTES);
    gameboy.next_frame_us = esp_timer_get_time();
    return true;
  }
  const uint8_t mask = gameboy_button_for_char(ch);
  if (mask != 0) {
    gameboy_press(mask, esp_timer_get_time());
  }
  return true;
}

static bool gameboy_run_frame(solar_os_context_t *ctx, int64_t now_us) {
  const int jump_result = setjmp(gameboy.core_error_jump);
  if (jump_result != 0) {
    gameboy.core_error_jump_ready = false;
    SOLAR_OS_LOGE(TAG, "core error=%d address=0x%04x", (int)gameboy.core_error,
                  (unsigned)gameboy.core_error_address);
    solar_os_context_set_status_message(ctx, "gameboy: emulator core error");
    solar_os_context_request_exit(ctx);
    return false;
  }

  gameboy.core_error_jump_ready = true;
  const bool draw_frame =
      gameboy.emulated_since_present + 1U >= GAMEBOY_PRESENT_DIVISOR;
  gameboy.core->display.lcd_draw_line =
      draw_frame ? gameboy_draw_line : NULL;
  gameboy.frame_rendered = false;
  const int64_t emulation_started = esp_timer_get_time();
  gb_run_frame(gameboy.core);
  const int64_t emulation_finished = esp_timer_get_time();
  gameboy.core_error_jump_ready = false;
  gameboy.emulation_us += (uint64_t)(emulation_finished - emulation_started);
  gameboy.stats_emulated_frames++;
  gameboy.emulated_since_present++;

  if (draw_frame) {
    if (gameboy.frame_rendered) {
      gameboy_render();
    }
    gameboy.emulated_since_present = 0;
  }
  gameboy_log_stats(emulation_finished);

  gameboy.next_frame_us += GAMEBOY_FRAME_PERIOD_US;
  if (emulation_finished - gameboy.next_frame_us > GAMEBOY_FRAME_PERIOD_US * 2) {
    gameboy.next_frame_us = emulation_finished;
  }
  (void)now_us;
  return true;
}

static bool gameboy_event(solar_os_context_t *ctx,
                          const solar_os_event_t *event) {
  if (event == NULL || !gameboy.loaded) {
    return false;
  }
  if (event->type == SOLAR_OS_EVENT_CHAR) {
    return gameboy_handle_char(ctx, (uint8_t)event->data.ch);
  }
  if (event->type == SOLAR_OS_EVENT_KEY) {
    const solar_os_input_key_event_t *key = &event->data.key;
    if (key->action == SOLAR_OS_INPUT_KEY_PRESS) {
      const uint8_t ch = key->key;
      if (key->physical_key == SOLAR_OS_INPUT_PHYSICAL_NONE ||
          ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE ||
          ch == 'q' || ch == 'p' || ch == 'r') {
        return gameboy_handle_char(ctx, ch);
      }
    }
    gameboy_refresh_inputs(esp_timer_get_time());
    return true;
  }
  if (event->type == SOLAR_OS_EVENT_RESUME) {
    gameboy_render();
    return true;
  }
  if (event->type != SOLAR_OS_EVENT_TICK || gameboy.suspended) {
    return false;
  }

  const int64_t now_us = esp_timer_get_time();
  if (gameboy.paused) {
    gameboy.core->direct.joypad = 0xFFU;
    return true;
  }
  gameboy_refresh_inputs(now_us);
  if (now_us < gameboy.next_frame_us) {
    return true;
  }
  return gameboy_run_frame(ctx, now_us);
}

static void gameboy_title(solar_os_context_t *ctx, char *buffer,
                          size_t buffer_len) {
  (void)ctx;
  if (buffer == NULL || buffer_len == 0) {
    return;
  }
  if (gameboy.title[0] != '\0') {
    snprintf(buffer, buffer_len, "Game Boy: %.16s", gameboy.title);
  } else {
    snprintf(buffer, buffer_len, "Game Boy");
  }
}

const solar_os_app_t solar_os_gameboy_app = {
    .name = "gameboy",
    .summary = "original Game Boy emulator",
    .flags = SOLAR_OS_APP_FLAG_KEY_EVENTS,
    .start = gameboy_start,
    .suspend = gameboy_suspend,
    .resume = gameboy_resume,
    .stop = gameboy_stop,
    .event = gameboy_event,
    .title = gameboy_title,
    .tick_interval_ms = 1U,
    .tick_deadline_ms = 120U,
};
