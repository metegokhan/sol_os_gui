#include "solar_os_gameboy_audio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "solar_os_log.h"
#include "solar_os_synth.h"

#define AUDIO_SAMPLE_RATE 16000
#define MINIGB_APU_AUDIO_FORMAT_S16SYS 1
#include "vendor/peanut_gb/minigb_apu.h"

#define GAMEBOY_AUDIO_OWNER "gameboy"

static const char *TAG = "solar_os_gameboy_audio";
static struct minigb_apu_ctx gameboy_apu;
static SemaphoreHandle_t gameboy_apu_mutex;
static StaticSemaphore_t gameboy_apu_mutex_storage;
static portMUX_TYPE gameboy_apu_init_lock = portMUX_INITIALIZER_UNLOCKED;
static bool gameboy_apu_initialized;
static bool gameboy_apu_running;

static esp_err_t gameboy_audio_ensure_mutex(void) {
  portENTER_CRITICAL(&gameboy_apu_init_lock);
  if (gameboy_apu_mutex == NULL) {
    gameboy_apu_mutex =
        xSemaphoreCreateMutexStatic(&gameboy_apu_mutex_storage);
  }
  SemaphoreHandle_t mutex = gameboy_apu_mutex;
  portEXIT_CRITICAL(&gameboy_apu_init_lock);
  return mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void gameboy_audio_lock(void) {
  (void)xSemaphoreTake(gameboy_apu_mutex, portMAX_DELAY);
}

static void gameboy_audio_unlock(void) {
  (void)xSemaphoreGive(gameboy_apu_mutex);
}

static void gameboy_audio_render(int16_t *samples, size_t frames,
                                 uint32_t sample_rate, void *user) {
  (void)user;
  if (samples == NULL || frames != AUDIO_SAMPLES ||
      sample_rate != AUDIO_SAMPLE_RATE || !gameboy_apu_initialized) {
    return;
  }
  gameboy_audio_lock();
  minigb_apu_audio_callback(&gameboy_apu, samples);
  gameboy_audio_unlock();
}

esp_err_t solar_os_gameboy_audio_resume(void) {
  if (!gameboy_apu_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (gameboy_apu_running) {
    solar_os_synth_status_t status;
    solar_os_synth_get_status(&status);
    if (status.running && strcmp(status.owner, GAMEBOY_AUDIO_OWNER) == 0) {
      return ESP_OK;
    }
    gameboy_apu_running = false;
  }
  const solar_os_synth_config_t config = {
      .owner = GAMEBOY_AUDIO_OWNER,
      .render = gameboy_audio_render,
      .user = NULL,
      .block_frames = AUDIO_SAMPLES,
  };
  const esp_err_t err = solar_os_synth_start(&config);
  if (err == ESP_OK) {
    gameboy_apu_running = true;
  }
  return err;
}

esp_err_t solar_os_gameboy_audio_init(void) {
  esp_err_t err = gameboy_audio_ensure_mutex();
  if (err != ESP_OK) {
    return err;
  }
  gameboy_audio_lock();
  minigb_apu_audio_init(&gameboy_apu);
  gameboy_apu_initialized = true;
  gameboy_audio_unlock();
  err = solar_os_gameboy_audio_resume();
  if (err != ESP_OK) {
    SOLAR_OS_LOGW(TAG, "output unavailable: %s", esp_err_to_name(err));
  }
  return err;
}

void solar_os_gameboy_audio_suspend(void) {
  if (!gameboy_apu_running) {
    return;
  }
  const esp_err_t err = solar_os_synth_stop(GAMEBOY_AUDIO_OWNER);
  if (err != ESP_OK) {
    SOLAR_OS_LOGW(TAG, "stop failed: %s", esp_err_to_name(err));
  } else {
    gameboy_apu_running = false;
  }
}

void solar_os_gameboy_audio_reset(void) {
  if (gameboy_apu_mutex == NULL) {
    return;
  }
  gameboy_audio_lock();
  minigb_apu_audio_init(&gameboy_apu);
  gameboy_audio_unlock();
}

void solar_os_gameboy_audio_deinit(void) {
  solar_os_gameboy_audio_suspend();
  if (!gameboy_apu_running && gameboy_apu_mutex != NULL) {
    gameboy_audio_lock();
    gameboy_apu_initialized = false;
    gameboy_audio_unlock();
  }
}

uint8_t solar_os_gameboy_audio_read(uint16_t address) {
  if (gameboy_apu_mutex == NULL || !gameboy_apu_initialized ||
      address < 0xFF10U || address > 0xFF3FU) {
    return 0xFFU;
  }
  gameboy_audio_lock();
  const uint8_t value = minigb_apu_audio_read(&gameboy_apu, address);
  gameboy_audio_unlock();
  return value;
}

void solar_os_gameboy_audio_write(uint16_t address, uint8_t value) {
  if (gameboy_apu_mutex == NULL || !gameboy_apu_initialized ||
      address < 0xFF10U || address > 0xFF3FU) {
    return;
  }
  gameboy_audio_lock();
  minigb_apu_audio_write(&gameboy_apu, address, value);
  gameboy_audio_unlock();
}
