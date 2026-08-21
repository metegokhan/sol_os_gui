#include "solar_os_capture.h"

#include <stdlib.h>
#include <string.h>

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "solar_os_gpio.h"

#define CAPTURE_RESOLUTION_HZ 1000000U /* 1 tick = 1 us */
#define CAPTURE_MEM_SYMBOLS   64U
#define CAPTURE_MAX_SYMBOLS   512U

static const char *TAG = "capture";

typedef struct {
    SemaphoreHandle_t done;
    StaticSemaphore_t done_buf;
    size_t num_symbols;
} capture_rx_ctx_t;

static bool IRAM_ATTR capture_rx_done(rmt_channel_handle_t chan,
                                      const rmt_rx_done_event_data_t *edata,
                                      void *user)
{
    capture_rx_ctx_t *ctx = (capture_rx_ctx_t *)user;
    BaseType_t hp = pdFALSE;
    ctx->num_symbols = edata != NULL ? edata->num_symbols : 0U;
    xSemaphoreGiveFromISR(ctx->done, &hp);
    return hp == pdTRUE;
}

esp_err_t solar_os_capture_record(int pin,
                                  uint32_t timeout_ms,
                                  uint32_t idle_us,
                                  solar_os_capture_pulse_t *out,
                                  size_t max,
                                  size_t *out_count)
{
    if (out == NULL || max == 0U || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0U;
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (idle_us == 0U) {
        idle_us = 12000U; /* 12 ms inter-frame gap default */
    }

    capture_rx_ctx_t ctx = {0};
    ctx.done = xSemaphoreCreateBinaryStatic(&ctx.done_buf);
    if (ctx.done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    rmt_channel_handle_t rx = NULL;
    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = CAPTURE_RESOLUTION_HZ,
        .mem_block_symbols = CAPTURE_MEM_SYMBOLS,
        .gpio_num = (gpio_num_t)pin,
    };
    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &rx);
    if (err != ESP_OK) {
        goto cleanup_sem;
    }

    const rmt_rx_event_callbacks_t cbs = { .on_recv_done = capture_rx_done };
    err = rmt_rx_register_event_callbacks(rx, &cbs, &ctx);
    if (err != ESP_OK) {
        goto cleanup_chan;
    }
    err = rmt_enable(rx);
    if (err != ESP_OK) {
        goto cleanup_chan;
    }

    /* Transient buffer (not static, so it doesn't sit in scarce internal .bss). */
    rmt_symbol_word_t *symbols = malloc(sizeof(rmt_symbol_word_t) * CAPTURE_MAX_SYMBOLS);
    if (symbols == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup_enabled;
    }
    const rmt_receive_config_t rcfg = {
        .signal_range_min_ns = 1000U,          /* ignore <1 us glitches */
        .signal_range_max_ns = idle_us * 1000U, /* stop after the idle gap */
    };
    err = rmt_receive(rx, symbols, sizeof(rmt_symbol_word_t) * CAPTURE_MAX_SYMBOLS, &rcfg);
    if (err != ESP_OK) {
        free(symbols);
        goto cleanup_enabled;
    }

    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        free(symbols);
        err = ESP_ERR_TIMEOUT;
        goto cleanup_enabled;
    }

    /* Flatten RMT symbols (two half-pulses each) into a level/duration list. */
    size_t n = 0;
    for (size_t i = 0; i < ctx.num_symbols && n < max; i++) {
        if (symbols[i].duration0 == 0U) {
            break;
        }
        out[n].level = (uint8_t)symbols[i].level0;
        out[n].duration_us = symbols[i].duration0;
        n++;
        if (symbols[i].duration1 == 0U || n >= max) {
            break;
        }
        out[n].level = (uint8_t)symbols[i].level1;
        out[n].duration_us = symbols[i].duration1;
        n++;
    }
    *out_count = n;
    err = n > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;

cleanup_enabled:
    (void)rmt_disable(rx);
cleanup_chan:
    (void)rmt_del_channel(rx);
cleanup_sem:
    vSemaphoreDelete(ctx.done);
    return err;
}

esp_err_t solar_os_capture_send(int pin,
                                const solar_os_capture_pulse_t *pulses,
                                size_t count,
                                uint32_t carrier_hz)
{
    if (pulses == NULL || count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }

    rmt_channel_handle_t tx = NULL;
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = CAPTURE_RESOLUTION_HZ,
        .mem_block_symbols = CAPTURE_MEM_SYMBOLS,
        .trans_queue_depth = 1,
        .gpio_num = (gpio_num_t)pin,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &tx);
    if (err != ESP_OK) {
        return err;
    }

    if (carrier_hz > 0U) {
        const rmt_carrier_config_t carrier = {
            .frequency_hz = carrier_hz,
            .duty_cycle = 0.33f,
        };
        (void)rmt_apply_carrier(tx, &carrier);
    }

    rmt_encoder_handle_t encoder = NULL;
    const rmt_copy_encoder_config_t enc_cfg = {0};
    err = rmt_new_copy_encoder(&enc_cfg, &encoder);
    if (err != ESP_OK) {
        goto cleanup_chan;
    }

    /* Pack the pulse list into RMT symbols (two half-pulses per symbol). */
    const size_t sym_count = (count + 1U) / 2U;
    if (sym_count > CAPTURE_MAX_SYMBOLS) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup_encoder;
    }
    rmt_symbol_word_t *tx_symbols = calloc(sym_count, sizeof(rmt_symbol_word_t));
    if (tx_symbols == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup_encoder;
    }
    for (size_t i = 0; i < count; i++) {
        rmt_symbol_word_t *s = &tx_symbols[i / 2U];
        if ((i & 1U) == 0U) {
            s->level0 = pulses[i].level & 1U;
            s->duration0 = pulses[i].duration_us ? pulses[i].duration_us : 1U;
        } else {
            s->level1 = pulses[i].level & 1U;
            s->duration1 = pulses[i].duration_us ? pulses[i].duration_us : 1U;
        }
    }

    err = rmt_enable(tx);
    if (err != ESP_OK) {
        free(tx_symbols);
        goto cleanup_encoder;
    }
    const rmt_transmit_config_t xcfg = { .loop_count = 0 };
    err = rmt_transmit(tx, encoder, tx_symbols,
                       sizeof(rmt_symbol_word_t) * sym_count, &xcfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(tx, 1000);
    }
    (void)rmt_disable(tx);
    free(tx_symbols);

cleanup_encoder:
    (void)rmt_del_encoder(encoder);
cleanup_chan:
    (void)rmt_del_channel(tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send failed: %s", esp_err_to_name(err));
    }
    return err;
}
