#include "solar_os_gameboy_presenter.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_display.h"
#include "solar_os_gameboy_video.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define GAMEBOY_DISPLAY_TARGET "display0"
#define GAMEBOY_DISPLAY_HPM_HZ_TENTHS 255U
#define GAMEBOY_PRESENTER_STACK 3072U
#define GAMEBOY_PRESENTER_PRIORITY (tskIDLE_PRIORITY + 1U)

typedef struct {
  solar_os_gfx_t *gfx;
  uint8_t *bitmap;
  SemaphoreHandle_t requested;
  StaticSemaphore_t requested_storage;
  TaskHandle_t task;
  volatile bool stop_requested;
  volatile bool task_done;
  bool busy;
  bool high_refresh_active;
  int x;
  int y;
  solar_os_gameboy_presenter_stats_t stats;
} gameboy_presenter_state_t;

static const char *TAG = "solar_os_gameboy_presenter";
static gameboy_presenter_state_t presenter;
static portMUX_TYPE presenter_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t gameboy_present_frame(void) {
  return solar_os_gfx_present_mono_xbm(
      presenter.gfx, presenter.bitmap, SOLAR_OS_GAMEBOY_BITMAP_BYTES,
      presenter.x, presenter.y, (int)SOLAR_OS_GAMEBOY_BITMAP_WIDTH,
      (int)SOLAR_OS_GAMEBOY_BITMAP_HEIGHT, SOLAR_OS_GAMEBOY_BITMAP_STRIDE);
}

static void gameboy_presenter_worker(void *arg) {
  (void)arg;
  while (true) {
    (void)xSemaphoreTake(presenter.requested, portMAX_DELAY);
    portENTER_CRITICAL(&presenter_lock);
    const bool stop = presenter.stop_requested;
    portEXIT_CRITICAL(&presenter_lock);
    if (stop) {
      break;
    }

    const int64_t started_us = esp_timer_get_time();
    const esp_err_t err = gameboy_present_frame();
    const uint64_t elapsed_us =
        (uint64_t)(esp_timer_get_time() - started_us);

    portENTER_CRITICAL(&presenter_lock);
    presenter.stats.last_error = err;
    if (err == ESP_OK) {
      presenter.stats.present_us += elapsed_us;
      presenter.stats.presented_frames++;
    }
    presenter.busy = false;
    portEXIT_CRITICAL(&presenter_lock);
    if (err != ESP_OK) {
      SOLAR_OS_LOGW(TAG, "present failed: %s", esp_err_to_name(err));
    }
  }

  portENTER_CRITICAL(&presenter_lock);
  presenter.busy = false;
  presenter.task_done = true;
  portEXIT_CRITICAL(&presenter_lock);
  solar_os_task_delete_internal(NULL);
}

esp_err_t solar_os_gameboy_presenter_resume(void) {
  if (presenter.gfx == NULL || presenter.bitmap == NULL ||
      presenter.requested == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (presenter.task != NULL) {
    return ESP_OK;
  }

  esp_err_t refresh_err = solar_os_display_set_high_refresh_override(
      GAMEBOY_DISPLAY_TARGET, true, GAMEBOY_DISPLAY_HPM_HZ_TENTHS);
  if (refresh_err == ESP_OK) {
    presenter.high_refresh_active = true;
  } else {
    SOLAR_OS_LOGW(TAG, "25.5 Hz HPM unavailable: %s",
                  esp_err_to_name(refresh_err));
  }

  while (xSemaphoreTake(presenter.requested, 0) == pdTRUE) {
  }
  portENTER_CRITICAL(&presenter_lock);
  presenter.stop_requested = false;
  presenter.task_done = false;
  presenter.busy = false;
  portEXIT_CRITICAL(&presenter_lock);
  const BaseType_t created = solar_os_task_create_pinned_internal(
      gameboy_presenter_worker, "gb_present", GAMEBOY_PRESENTER_STACK, NULL,
      GAMEBOY_PRESENTER_PRIORITY, &presenter.task, tskNO_AFFINITY,
      SOLAR_OS_TASK_ROLE_FOREGROUND);
  if (created != pdPASS) {
    presenter.task = NULL;
    if (presenter.high_refresh_active) {
      (void)solar_os_display_set_high_refresh_override(
          GAMEBOY_DISPLAY_TARGET, false, 0);
      presenter.high_refresh_active = false;
    }
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

esp_err_t solar_os_gameboy_presenter_init(solar_os_gfx_t *gfx) {
  if (gfx == NULL ||
      solar_os_gfx_width(gfx) < SOLAR_OS_GAMEBOY_BITMAP_WIDTH ||
      solar_os_gfx_height(gfx) < SOLAR_OS_GAMEBOY_BITMAP_HEIGHT) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(&presenter, 0, sizeof(presenter));
  presenter.gfx = gfx;
  presenter.x =
      ((int)solar_os_gfx_width(gfx) - (int)SOLAR_OS_GAMEBOY_BITMAP_WIDTH) / 2;
  presenter.y =
      ((int)solar_os_gfx_height(gfx) - (int)SOLAR_OS_GAMEBOY_BITMAP_HEIGHT) / 2;
  presenter.bitmap = solar_os_memory_alloc(
      SOLAR_OS_GAMEBOY_BITMAP_BYTES, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
      "gameboy.present");
  if (presenter.bitmap == NULL) {
    return ESP_ERR_NO_MEM;
  }
  memset(presenter.bitmap, 0, SOLAR_OS_GAMEBOY_BITMAP_BYTES);
  presenter.requested =
      xSemaphoreCreateBinaryStatic(&presenter.requested_storage);
  if (presenter.requested == NULL) {
    solar_os_memory_free(presenter.bitmap);
    presenter.bitmap = NULL;
    return ESP_ERR_NO_MEM;
  }
  return solar_os_gameboy_presenter_resume();
}

void solar_os_gameboy_presenter_suspend(void) {
  TaskHandle_t task = presenter.task;
  if (task != NULL) {
    portENTER_CRITICAL(&presenter_lock);
    presenter.stop_requested = true;
    portEXIT_CRITICAL(&presenter_lock);
    (void)xSemaphoreGive(presenter.requested);
    if (!solar_os_task_wait_done(task, &presenter.task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
      SOLAR_OS_LOGW(TAG, "present worker stop timed out");
      return;
    }
    presenter.task = NULL;
  }
  if (presenter.high_refresh_active) {
    const esp_err_t err = solar_os_display_set_high_refresh_override(
        GAMEBOY_DISPLAY_TARGET, false, 0);
    if (err != ESP_OK) {
      SOLAR_OS_LOGW(TAG, "display policy restore failed: %s",
                    esp_err_to_name(err));
    } else {
      presenter.high_refresh_active = false;
    }
  }
}

void solar_os_gameboy_presenter_deinit(void) {
  solar_os_gameboy_presenter_suspend();
  if (presenter.task != NULL) {
    return;
  }
  if (presenter.requested != NULL) {
    vSemaphoreDelete(presenter.requested);
  }
  solar_os_memory_free(presenter.bitmap);
  memset(&presenter, 0, sizeof(presenter));
}

bool solar_os_gameboy_presenter_queue(const uint8_t *bitmap) {
  if (bitmap == NULL || presenter.bitmap == NULL || presenter.task == NULL) {
    return false;
  }
  portENTER_CRITICAL(&presenter_lock);
  if (presenter.busy || presenter.stop_requested) {
    presenter.stats.dropped_frames++;
    portEXIT_CRITICAL(&presenter_lock);
    return false;
  }
  presenter.busy = true;
  portEXIT_CRITICAL(&presenter_lock);

  memcpy(presenter.bitmap, bitmap, SOLAR_OS_GAMEBOY_BITMAP_BYTES);
  (void)xSemaphoreGive(presenter.requested);
  return true;
}

void solar_os_gameboy_presenter_take_stats(
    solar_os_gameboy_presenter_stats_t *stats) {
  if (stats == NULL) {
    return;
  }
  portENTER_CRITICAL(&presenter_lock);
  *stats = presenter.stats;
  memset(&presenter.stats, 0, sizeof(presenter.stats));
  portEXIT_CRITICAL(&presenter_lock);
}
