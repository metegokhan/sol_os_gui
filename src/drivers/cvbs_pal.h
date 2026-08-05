#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_rom_lldesc.h"
#include "freertos/FreeRTOS.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SOLAR_OS_CVBS_MODE_320X200
#define SOLAR_OS_CVBS_MODE_320X200 0
#endif

#if SOLAR_OS_CVBS_MODE_320X200
#define CVBS_PAL_WIDTH 320U
#define CVBS_PAL_HEIGHT 200U
#else
#define CVBS_PAL_WIDTH 384U
#define CVBS_PAL_HEIGHT 288U
#endif

typedef struct {
    u8g2_t u8g2;
    uint8_t *draw_buffer;
    uint8_t *scanout_buffers[2];
    uint8_t *dma_buffer;
    size_t buffer_size;
    size_t dma_buffer_size;
    lldesc_t dma_desc[2];
    intr_handle_t interrupt;
    portMUX_TYPE buffer_lock;
    volatile uint16_t next_scanline;
    volatile int8_t current_buffer;
    volatile int8_t pending_buffer;
    volatile int8_t copying_buffer;
    esp_err_t last_error;
    bool signal_started;
} cvbs_pal_t;

esp_err_t cvbs_pal_init(cvbs_pal_t *display);
esp_err_t cvbs_pal_resume(cvbs_pal_t *display);
void cvbs_pal_deinit(cvbs_pal_t *display);
u8g2_t *cvbs_pal_get_u8g2(cvbs_pal_t *display);

#ifdef __cplusplus
}
#endif
