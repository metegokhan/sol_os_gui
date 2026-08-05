/*
 * SolarOS monochrome PAL scanout for the original ESP32 DAC.
 *
 * The 625-line timing, field sync table, APLL coefficients, and I2S/DAC
 * setup are adapted from LovyanGFX Panel_CVBS:
 * https://github.com/lovyan03/LovyanGFX
 *
 * LovyanGFX is distributed under the FreeBSD license. Copyright (c) lovyan03
 * and contributors. The original implementation also credits Roger Cheng's
 * ESP_8_BIT_composite and rossumur's esp_8_bit projects.
 */

#include "cvbs_pal.h"

#include <string.h>

#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/periph_ctrl.h"
#include "hal/dac_ll.h"
#include "hal/dac_types.h"
#include "soc/i2s_struct.h"
#include "soc/periph_defs.h"
#include "soc/rtc.h"
#include "solar_os_board.h"

#define CVBS_NATIVE_WIDTH CVBS_PAL_HEIGHT
#define CVBS_NATIVE_HEIGHT CVBS_PAL_WIDTH
#define CVBS_TILE_WIDTH ((CVBS_NATIVE_WIDTH + 7U) / 8U)
#define CVBS_TILE_HEIGHT ((CVBS_NATIVE_HEIGHT + 7U) / 8U)
#define CVBS_BUFFER_SIZE (CVBS_TILE_WIDTH * CVBS_TILE_HEIGHT * 8U)

#if SOLAR_OS_CVBS_MODE_320X200
/*
 * Conservative non-interlaced PAL timing for small displays. The 320x200
 * canvas is centered in the 312-line frame and uses one DAC sample per pixel.
 */
#define PAL_TOTAL_SCANLINES 312U
#define PAL_SCANLINE_SAMPLES 472U
#define PAL_SYNC_SAMPLES 35U
#define PAL_SHORT_SYNC_SAMPLES 17U
#define PAL_LONG_SYNC_SAMPLES 201U
#define PAL_VISIBLE_START 66U
#define PAL_VISIBLE_END (PAL_VISIBLE_START + CVBS_PAL_HEIGHT)
#define PAL_TRAILING_SYNC_START 309U
#define PAL_MONO_ACTIVE_START 108U
#define CVBS_LEVEL_SYNC 0U
#define CVBS_LEVEL_BLANKING 23U
#define CVBS_LEVEL_BLACK 23U
#define CVBS_LEVEL_WHITE 77U
#else
#define PAL_TOTAL_SCANLINES 625U
#define PAL_FIELD_START 312U
#define PAL_SCANLINE_SAMPLES 1136U
#define PAL_VSYNC_LINES 25U
#define PAL_SYNC_SAMPLES 84U
#define PAL_EQUALIZING_SAMPLES 40U
#define PAL_LONG_SYNC_SAMPLES 484U
#define PAL_ACTIVE_START 216U
#define PAL_MONO_ACTIVE_START (PAL_ACTIVE_START + 48U)
/* LovyanGFX's direct ESP32 DAC levels at the default CVBS output strength. */
#define CVBS_LEVEL_SYNC 0U
#define CVBS_LEVEL_BLANKING 28U
#define CVBS_LEVEL_BLACK 28U
#define CVBS_LEVEL_WHITE 90U
#endif
#define CVBS_WORD(level) ((uint16_t)((level) | ((level) << 8U)))
#define CVBS_DWORD(level) ((uint32_t)CVBS_WORD(level) | ((uint32_t)CVBS_WORD(level) << 16U))
#define CVBS_PIXEL_PAIR(first, second) \
    (((uint32_t)(first) << 24U) | ((uint32_t)(second) << 8U))

static const char *TAG = "cvbs-pal";
static cvbs_pal_t *active_display;
static DRAM_ATTR uint32_t pixel_lut[256][8];
static bool pixel_lut_ready;

/* LovyanGFX PAL field-sync description. Kept in DRAM for the IRAM ISR. */
static DRAM_ATTR const uint8_t pal_sync_proc[2][12] = {
    {0x05, 0x55, 0x50, 0x22, 0x22, 0x05, 0x55, 0x50, 0x34, 0xB0, 0xB0, 0x00},
    {0x00, 0x55, 0x55, 0x02, 0x22, 0x20, 0x55, 0x55, 0x04, 0xB0, 0xB0, 0x00},
};

static const u8x8_display_info_t cvbs_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .sck_clock_hz = 17734476UL,
    .i2c_bus_clock_100kHz = 0,
    .tile_width = CVBS_TILE_WIDTH,
    .tile_height = CVBS_TILE_HEIGHT,
    .pixel_width = CVBS_NATIVE_WIDTH,
    .pixel_height = CVBS_NATIVE_HEIGHT,
};

static inline void IRAM_ATTR fill_samples(uint16_t *buffer,
                                          size_t start,
                                          size_t count,
                                          uint8_t level)
{
    memset(&buffer[start], level, count * sizeof(uint16_t));
}

static inline void IRAM_ATTR render_pixels(cvbs_pal_t *display,
                                           uint16_t *buffer,
                                           uint16_t y)
{
    const uint8_t *frame = display->scanout_buffers[display->current_buffer];
    const uint8_t *row = &frame[(size_t)y * (CVBS_PAL_WIDTH / 8U)];
    uint32_t *output = (uint32_t *)&buffer[PAL_MONO_ACTIVE_START];

#if SOLAR_OS_CVBS_MODE_320X200
    /* I2S LCD mode swaps adjacent 16-bit samples, so each LUT word stores a
     * pair in transmission order. Four aligned writes render eight pixels. */
    for (size_t group = 0; group < CVBS_PAL_WIDTH / 8U; group++) {
        const uint32_t *pixels = pixel_lut[row[group]];
        output[0] = pixels[0];
        output[1] = pixels[1];
        output[2] = pixels[2];
        output[3] = pixels[3];
        output += 4;
    }
#else
    /*
     * Two identical samples per pixel are immune to I2S LCD mode's adjacent
     * 16-bit swap. The row-major buffer and lookup table keep this ISR to 48
     * byte reads and 384 aligned 32-bit writes per visible scanline.
     */
    for (size_t group = 0; group < CVBS_PAL_WIDTH / 8U; group++) {
        const uint32_t *pixels = pixel_lut[row[group]];
        output[0] = pixels[0];
        output[1] = pixels[1];
        output[2] = pixels[2];
        output[3] = pixels[3];
        output[4] = pixels[4];
        output[5] = pixels[5];
        output[6] = pixels[6];
        output[7] = pixels[7];
        output += 8;
    }
#endif
}

static inline void IRAM_ATTR render_normal_line(cvbs_pal_t *display,
                                                uint16_t *buffer,
                                                int y)
{
    fill_samples(buffer, 0, PAL_SYNC_SAMPLES, CVBS_LEVEL_SYNC);
    fill_samples(buffer,
                 PAL_SYNC_SAMPLES,
                 PAL_SCANLINE_SAMPLES - PAL_SYNC_SAMPLES,
                 CVBS_LEVEL_BLACK);
    if (y >= 0 && y < (int)CVBS_PAL_HEIGHT) {
        render_pixels(display, buffer, (uint16_t)y);
    }
}

static inline void IRAM_ATTR render_vsync_line(uint16_t *buffer,
                                               bool odd_field,
                                               uint16_t field_line)
{
#if SOLAR_OS_CVBS_MODE_320X200
    (void)odd_field;
    const bool first_long = field_line == 0U || field_line == 1U ||
                            field_line == 2U;
    const bool second_long = field_line == 0U || field_line == 1U;
    const size_t half = PAL_SCANLINE_SAMPLES / 2U;
    const size_t first_width = first_long
                                   ? PAL_LONG_SYNC_SAMPLES
                                   : PAL_SHORT_SYNC_SAMPLES;
    const size_t second_width = second_long
                                    ? PAL_LONG_SYNC_SAMPLES
                                    : PAL_SHORT_SYNC_SAMPLES;

    fill_samples(buffer, 0, first_width, CVBS_LEVEL_SYNC);
    fill_samples(buffer,
                 first_width,
                 half - first_width,
                 CVBS_LEVEL_BLANKING);
    fill_samples(buffer, half, second_width, CVBS_LEVEL_SYNC);
    fill_samples(buffer,
                 half + second_width,
                 half - second_width,
                 CVBS_LEVEL_BLANKING);
#else
    /*
     * The LovyanGFX sync table contains edits to the waveform that was last
     * sent through this DMA descriptor, not complete scanlines. Each of our
     * two descriptors returns here two scanlines later, which preserves that
     * state. Rebuilding the complete line here removes broad/equalizing pulse
     * sections and prevents some displays from locking to vertical sync.
     */
    if (field_line >= 12U) {
        return;
    }

    const uint8_t proc = pal_sync_proc[odd_field ? 1 : 0][field_line];
    const size_t half = PAL_SCANLINE_SAMPLES / 2U;

    if ((proc & 0x40U) != 0U) {
        fill_samples(buffer, 0, half, CVBS_LEVEL_BLANKING);
        buffer[(half - 1U) ^ 1U] = CVBS_WORD(CVBS_LEVEL_BLANKING);
    }
    if ((proc & 0x04U) != 0U) {
        const size_t blank_start = (half + 1U) & ~1U;
        fill_samples(buffer,
                     blank_start,
                     PAL_SCANLINE_SAMPLES - blank_start,
                     CVBS_LEVEL_BLANKING);
        buffer[half ^ 1U] = CVBS_WORD(CVBS_LEVEL_BLANKING);
    }
    if ((proc & 0x03U) != 0U) {
        const size_t width = (proc & 0x01U) != 0U
                                 ? PAL_EQUALIZING_SAMPLES
                                 : PAL_LONG_SYNC_SAMPLES;
        const size_t pulse_start = (half + 1U) & ~1U;
        fill_samples(buffer, pulse_start, width, CVBS_LEVEL_SYNC);
        buffer[half ^ 1U] = CVBS_WORD(CVBS_LEVEL_SYNC);
    }
    if ((proc & 0x30U) != 0U) {
        size_t width = PAL_EQUALIZING_SAMPLES;
        switch ((proc >> 4U) & 0x03U) {
        case 2:
            width = PAL_LONG_SYNC_SAMPLES;
            break;
        case 3:
            width = PAL_SYNC_SAMPLES;
            break;
        default:
            break;
        }
        fill_samples(buffer, 0, width, CVBS_LEVEL_SYNC);
    }
    if ((proc & 0x80U) != 0U) {
        /* Monochrome CVBS intentionally has no color burst. */
        fill_samples(buffer,
                     PAL_ACTIVE_START,
                     PAL_SCANLINE_SAMPLES - 22U - PAL_ACTIVE_START,
                     CVBS_LEVEL_BLACK);
    }
#endif
}

static inline void IRAM_ATTR accept_pending_frame(cvbs_pal_t *display)
{
    portENTER_CRITICAL_ISR(&display->buffer_lock);
    if (display->pending_buffer >= 0 &&
        display->pending_buffer != display->current_buffer) {
        display->current_buffer = display->pending_buffer;
        display->pending_buffer = -1;
    }
    portEXIT_CRITICAL_ISR(&display->buffer_lock);
}

static void IRAM_ATTR render_scanline(cvbs_pal_t *display,
                                      uint16_t *buffer,
                                      uint16_t scanline)
{
#if SOLAR_OS_CVBS_MODE_320X200
    if (scanline == 0U) {
        accept_pending_frame(display);
    }

    if (scanline < 5U || scanline >= PAL_TRAILING_SYNC_START) {
        render_vsync_line(buffer, false, scanline < 5U ? scanline : 4U);
    } else if (scanline >= PAL_VISIBLE_START && scanline < PAL_VISIBLE_END) {
        render_normal_line(display,
                           buffer,
                           (int)(scanline - PAL_VISIBLE_START));
    } else {
        render_normal_line(display, buffer, -1);
    }
#else
    if (scanline == 0U || scanline == PAL_FIELD_START) {
        accept_pending_frame(display);
    }

    const bool odd_field = scanline >= PAL_FIELD_START;
    const uint16_t field_line = odd_field
                                    ? (uint16_t)(scanline - PAL_FIELD_START)
                                    : scanline;
    if (field_line < PAL_VSYNC_LINES) {
        render_vsync_line(buffer, odd_field, field_line);
        return;
    }

    const int y = (int)field_line - (int)PAL_VSYNC_LINES;
    if (y >= 0 && y < (int)CVBS_PAL_HEIGHT) {
        render_pixels(display, buffer, (uint16_t)y);
    } else if (y < (int)CVBS_PAL_HEIGHT + 2) {
        /* Clear both alternating descriptors after the last visible row. */
        fill_samples(buffer,
                     PAL_MONO_ACTIVE_START,
                     CVBS_PAL_WIDTH * 2U,
                     CVBS_LEVEL_BLACK);
    }
#endif
}

static void IRAM_ATTR cvbs_i2s_isr(void *arg)
{
    cvbs_pal_t *display = (cvbs_pal_t *)arg;
    const bool eof = I2S0.int_st.out_eof;
    I2S0.int_clr.val = I2S0.int_st.val;
    if (!eof) {
        return;
    }

    lldesc_t *descriptor = (lldesc_t *)I2S0.out_eof_des_addr;
    render_scanline(display, (uint16_t *)descriptor->buf, display->next_scanline);
    display->next_scanline++;
    if (display->next_scanline >= PAL_TOTAL_SCANLINES) {
        display->next_scanline = 0;
    }
}

static esp_err_t cvbs_present(cvbs_pal_t *display)
{
    int8_t target = -1;
    portENTER_CRITICAL(&display->buffer_lock);
    if (display->copying_buffer < 0) {
        target = display->pending_buffer >= 0
                     ? display->pending_buffer
                     : (int8_t)(1 - display->current_buffer);
        display->pending_buffer = -1;
        display->copying_buffer = target;
    }
    portEXIT_CRITICAL(&display->buffer_lock);

    if (target < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *scanout = display->scanout_buffers[target];
    memset(scanout, 0, display->buffer_size);
    for (size_t y = 0; y < CVBS_PAL_HEIGHT; y++) {
        uint8_t *row = &scanout[y * (CVBS_PAL_WIDTH / 8U)];
        const size_t native_x = CVBS_NATIVE_WIDTH - 1U - y;
        for (size_t x = 0; x < CVBS_PAL_WIDTH; x++) {
            const size_t native_y = x;
            const uint8_t *source =
                &display->draw_buffer[(native_y >> 3U) * CVBS_NATIVE_WIDTH];
            const uint8_t source_mask = (uint8_t)(1U << (native_y & 7U));
            if ((source[native_x] & source_mask) != 0U) {
                row[x >> 3U] |= (uint8_t)(0x80U >> (x & 7U));
            }
        }
    }

    portENTER_CRITICAL(&display->buffer_lock);
    if (display->copying_buffer == target) {
        display->copying_buffer = -1;
        display->pending_buffer = target;
    }
    portEXIT_CRITICAL(&display->buffer_lock);
    return ESP_OK;
}

static void cvbs_stop_signal(cvbs_pal_t *display)
{
    if (display == NULL || !display->signal_started) {
        return;
    }

    display->signal_started = false;
    if (display->interrupt != NULL) {
        (void)esp_intr_disable(display->interrupt);
    }
    for (size_t i = 0; i < 2; i++) {
        display->dma_desc[i].empty = 0;
    }
    if (display->interrupt != NULL) {
        (void)esp_intr_free(display->interrupt);
        display->interrupt = NULL;
    }

    I2S0.out_link.stop = 1;
    I2S0.out_link.start = 0;
    I2S0.conf.tx_start = 0;
    dac_ll_digi_enable_dma(false);
    dac_ll_power_down(DAC_CHAN_0);
    periph_module_disable(PERIPH_I2S0_MODULE);
    rtc_clk_apll_enable(false);

    heap_caps_free(display->dma_buffer);
    display->dma_buffer = NULL;
    display->dma_buffer_size = 0;
}

static esp_err_t cvbs_start_signal(cvbs_pal_t *display)
{
    if (display == NULL || display->scanout_buffers[0] == NULL ||
        display->scanout_buffers[1] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (display->signal_started) {
        return ESP_OK;
    }
    if (SOLAR_OS_BOARD_PIN_COMPOSITE_VIDEO != GPIO_NUM_25) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const size_t line_bytes = PAL_SCANLINE_SAMPLES * sizeof(uint16_t);
    display->dma_buffer_size = line_bytes * 2U;
    display->dma_buffer = heap_caps_calloc(1,
                                           display->dma_buffer_size,
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (display->dma_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* From here on, cvbs_stop_signal() owns all partial-start cleanup. */
    display->signal_started = true;

    for (size_t i = 0; i < 2; i++) {
        lldesc_t *descriptor = &display->dma_desc[i];
        memset(descriptor, 0, sizeof(*descriptor));
        descriptor->buf = &display->dma_buffer[i * line_bytes];
        descriptor->owner = 1;
        descriptor->eof = 1;
        descriptor->length = line_bytes;
        descriptor->size = line_bytes;
        descriptor->empty = (uint32_t)&display->dma_desc[(i + 1U) & 1U];
    }

    /* Seed both descriptors before the DMA engine starts. */
    render_normal_line(display, (uint16_t *)display->dma_desc[0].buf, -1);
    render_normal_line(display, (uint16_t *)display->dma_desc[1].buf, -1);
    display->next_scanline = 0;
    render_scanline(display, (uint16_t *)display->dma_desc[0].buf, display->next_scanline++);
    render_scanline(display, (uint16_t *)display->dma_desc[1].buf, display->next_scanline++);

    esp_err_t err = rtc_gpio_init(GPIO_NUM_25);
    if (err != ESP_OK) {
        cvbs_stop_signal(display);
        return err;
    }
    err = rtc_gpio_set_direction(GPIO_NUM_25, RTC_GPIO_MODE_DISABLED);
    if (err != ESP_OK) {
        cvbs_stop_signal(display);
        return err;
    }
    (void)rtc_gpio_pullup_dis(GPIO_NUM_25);
    (void)rtc_gpio_pulldown_dis(GPIO_NUM_25);
    dac_ll_power_on(DAC_CHAN_0);
    dac_ll_rtc_sync_by_adc(false);
    dac_ll_digi_enable_dma(true);

    periph_module_enable(PERIPH_I2S0_MODULE);
    err = esp_intr_alloc(ETS_I2S0_INTR_SOURCE,
                         ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM,
                         cvbs_i2s_isr,
                         display,
                         &display->interrupt);
    if (err != ESP_OK) {
        cvbs_stop_signal(display);
        return err;
    }

    rtc_clk_apll_enable(true);
#if SOLAR_OS_CVBS_MODE_320X200
    rtc_clk_apll_coeff_set(6, 0xCD, 0xCC, 0x07);
#else
    rtc_clk_apll_coeff_set(1, 0x04, 0xA4, 0x06);
#endif

#if SOLAR_OS_CVBS_MODE_320X200
    I2S0.conf.val = 1;
    I2S0.conf.val = 0;
#else
    I2S0.conf.tx_reset = 1;
    I2S0.conf.tx_reset = 0;
#endif
    I2S0.conf.tx_right_first = 1;
    I2S0.conf.tx_mono = 1;
    I2S0.conf.tx_msb_shift = 0;
    I2S0.conf.tx_short_sync = 0;
    I2S0.conf2.lcd_en = 1;
    I2S0.conf_chan.tx_chan_mod = 1;
    I2S0.sample_rate_conf.tx_bits_mod = 16;
    I2S0.sample_rate_conf.tx_bck_div_num = 1;
    I2S0.clkm_conf.clka_en = 1;
    I2S0.clkm_conf.clkm_div_num = 1;
    I2S0.clkm_conf.clkm_div_b = 0;
    I2S0.clkm_conf.clkm_div_a = 1;
    I2S0.fifo_conf.tx_fifo_mod = 1;
    I2S0.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S0.out_link.addr = (uint32_t)display->dma_desc;
#if SOLAR_OS_CVBS_MODE_320X200
    I2S0.conf.tx_start = 1;
#endif
    I2S0.out_link.start = 1;
    I2S0.int_clr.val = UINT32_MAX;
    I2S0.int_ena.out_eof = 1;

    err = esp_intr_enable(display->interrupt);
    if (err != ESP_OK) {
        cvbs_stop_signal(display);
        return err;
    }
#if !SOLAR_OS_CVBS_MODE_320X200
    I2S0.conf.tx_start = 1;
#endif
#if SOLAR_OS_CVBS_MODE_320X200
    ESP_LOGI(TAG,
             "PAL 312p/50 safe-area output on GPIO25, %ux%u",
             CVBS_PAL_WIDTH,
             CVBS_PAL_HEIGHT);
#else
    ESP_LOGI(TAG, "PAL 625/50 monochrome output on GPIO25, %ux%u", CVBS_PAL_WIDTH, CVBS_PAL_HEIGHT);
#endif
    return ESP_OK;
}

static uint8_t cvbs_u8x8_display_cb(u8x8_t *u8x8,
                                    uint8_t message,
                                    uint8_t arg_int,
                                    void *arg_ptr)
{
    (void)arg_ptr;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &cvbs_display_info);
        return 1;
    }

    cvbs_pal_t *display = active_display;
    if (display == NULL) {
        return 0;
    }

    esp_err_t err = ESP_OK;
    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        err = cvbs_start_signal(display);
        break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        if (arg_int != 0U) {
            cvbs_stop_signal(display);
        } else {
            err = cvbs_start_signal(display);
        }
        break;
    case U8X8_MSG_DISPLAY_DRAW_TILE:
        return 1;
    case U8X8_MSG_DISPLAY_REFRESH:
        err = cvbs_present(display);
        break;
    default:
        return 0;
    }

    display->last_error = err;
    return err == ESP_OK ? 1 : 0;
}

esp_err_t cvbs_pal_init(cvbs_pal_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));
    if (!pixel_lut_ready) {
        for (size_t pattern = 0; pattern < 256U; pattern++) {
#if SOLAR_OS_CVBS_MODE_320X200
            for (size_t pair = 0; pair < 4U; pair++) {
                const size_t first_bit = pair * 2U;
                const size_t second_bit = first_bit + 1U;
                const uint8_t first =
                    (pattern & (0x80U >> first_bit)) != 0U
                        ? CVBS_LEVEL_WHITE
                        : CVBS_LEVEL_BLACK;
                const uint8_t second =
                    (pattern & (0x80U >> second_bit)) != 0U
                        ? CVBS_LEVEL_WHITE
                        : CVBS_LEVEL_BLACK;
                pixel_lut[pattern][pair] = CVBS_PIXEL_PAIR(first, second);
            }
#else
            for (size_t bit = 0; bit < 8U; bit++) {
                const uint8_t level = (pattern & (0x80U >> bit)) != 0U
                                          ? CVBS_LEVEL_WHITE
                                          : CVBS_LEVEL_BLACK;
                pixel_lut[pattern][bit] = CVBS_DWORD(level);
            }
#endif
        }
        pixel_lut_ready = true;
    }
    display->buffer_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    display->current_buffer = 0;
    display->pending_buffer = -1;
    display->copying_buffer = -1;
    display->buffer_size = CVBS_BUFFER_SIZE;

    display->draw_buffer = heap_caps_calloc(1,
                                             display->buffer_size,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (size_t i = 0; i < 2; i++) {
        display->scanout_buffers[i] = heap_caps_calloc(1,
                                                       display->buffer_size,
                                                       MALLOC_CAP_INTERNAL |
                                                           MALLOC_CAP_8BIT);
    }
    if (display->draw_buffer == NULL || display->scanout_buffers[0] == NULL ||
        display->scanout_buffers[1] == NULL) {
        cvbs_pal_deinit(display);
        return ESP_ERR_NO_MEM;
    }

    u8g2_SetupDisplay(&display->u8g2,
                      cvbs_u8x8_display_cb,
                      u8x8_cad_empty,
                      u8x8_dummy_cb,
                      u8x8_dummy_cb);
    u8g2_SetupBuffer(&display->u8g2,
                     display->draw_buffer,
                     CVBS_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb,
                     SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION);
    active_display = display;
    u8g2_InitDisplay(&display->u8g2);
    if (display->last_error != ESP_OK) {
        const esp_err_t err = display->last_error;
        cvbs_pal_deinit(display);
        return err;
    }
    u8g2_ClearBuffer(&display->u8g2);
    return ESP_OK;
}

esp_err_t cvbs_pal_resume(cvbs_pal_t *display)
{
    if (display == NULL || display->draw_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    active_display = display;
    display->last_error = cvbs_start_signal(display);
    return display->last_error;
}

void cvbs_pal_deinit(cvbs_pal_t *display)
{
    if (display == NULL) {
        return;
    }
    cvbs_stop_signal(display);
    if (active_display == display) {
        active_display = NULL;
    }
    heap_caps_free(display->draw_buffer);
    display->draw_buffer = NULL;
    for (size_t i = 0; i < 2; i++) {
        heap_caps_free(display->scanout_buffers[i]);
        display->scanout_buffers[i] = NULL;
    }
}

u8g2_t *cvbs_pal_get_u8g2(cvbs_pal_t *display)
{
    return display != NULL ? &display->u8g2 : NULL;
}
