/*
 * Desibel - canli ses seviyesi / spektrum / spektrogram olcer
 * (SolarOS native uygulamasi)
 *
 * Bu dosya SolarOS kaynak agacinin DISINDA gelistirilmektedir
 * (bkz. ../../docs/solaros_native_app_notes.md ve ../../integration/README.md).
 * Kaynaga entegre edilene kadar derlenmez. Ham mikrofon PCM erisimi ve
 * gorev (task) deseni solar_os_recorder.c'den, FFT API'si
 * solar_os_dsp.h'den cikarilmistir.
 *
 * ONEMLI: burada hesaplanan "dB" degeri, kalibre edilmis bir SPL
 * (ses basinc seviyesi) olcumu DEGILDIR -- mikrofonun dijital tam
 * skalasina (dBFS) gore goreli bir seviyedir. Kullanici, elindeki
 * kalibre bir olcumle karsilastirip "calibration_offset_db" degerini
 * ayarlayarak yaklasik bir hizalama yapabilir (+/- tuslariyla).
 */

#include "solar_os_desibel.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "solar_os_appbar.h"
#include "solar_os_audio.h"
#include "solar_os_dsp.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_storage.h"
#include "solar_os_stream.h"
#include "solar_os_task.h"

#define TAG "desibel"

#define DESIBEL_SAMPLE_RATE 16000U
#define DESIBEL_CHANNELS 1U
#define DESIBEL_FFT_SIZE 256U
#define DESIBEL_BANDS 32U
#define DESIBEL_HISTORY_COLUMNS 96U
#define DESIBEL_TIME_HISTORY 200U

#define DESIBEL_TASK_STACK 8192U
#define DESIBEL_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)

#define DESIBEL_TICK_MS 80U
#define DESIBEL_DB_FLOOR (-90.0f)
#define DESIBEL_DB_CEIL 0.0f
#define DESIBEL_NORM_FLOOR 0.0000305f /* ~ -90 dBFS taban, log10(0) kacinma */
#define DESIBEL_PI 3.14159265358979323846f

#define DESIBEL_LOG_DIR "desibel"
#define DESIBEL_LOG_FILE "olcum.csv"

typedef enum {
    DESIBEL_VIEW_OVERVIEW = 0,
    DESIBEL_VIEW_SPECTRUM,
    DESIBEL_VIEW_SPECTROGRAM,
    DESIBEL_VIEW_COUNT,
} desibel_view_t;

typedef struct {
    /* --- ses gorevi paylasimli alanlari (desibel_lock ile korunur) --- */
    int16_t staging[DESIBEL_FFT_SIZE];
    size_t staging_count;
    bool staging_ready;
    uint64_t level_sum_sq;
    uint32_t level_sample_count;
    int16_t level_peak_abs;

    /* --- gorev yasam dongusu --- */
    TaskHandle_t task;
    volatile bool task_done;
    volatile bool stop_requested;
    bool monitoring;

    /* --- FFT --- */
    solar_os_dsp_fft_t *fft;
    int16_t window_q15[DESIBEL_FFT_SIZE];
    int16_t windowed[DESIBEL_FFT_SIZE];
    solar_os_dsp_complex_s16_t spectrum[DESIBEL_FFT_SIZE];
    float band_db[DESIBEL_BANDS];
    float band_peak_db[DESIBEL_BANDS];

    /* --- spektrogram gecmisi (dairesel, 0..15 nicelenmis seviye) --- */
    uint8_t spectrogram[DESIBEL_HISTORY_COLUMNS][DESIBEL_BANDS];
    size_t spectrogram_head;
    size_t spectrogram_count;

    /* --- zaman-serisi dB gecmisi --- */
    float level_history[DESIBEL_TIME_HISTORY];
    size_t level_head;
    size_t level_count;

    /* --- anlik degerler --- */
    float current_db;
    float peak_db;
    float dominant_hz;
    float min_db_session;
    float max_db_session;
    bool have_session_stats;
    float calibration_offset_db;

    /* --- gorunum / durum --- */
    desibel_view_t view;
    bool logging;
    FILE *log_file;
    uint32_t log_last_flush_ms;

    uint32_t elapsed_ms;
    bool render_pending;
    char status_message[64];
    uint32_t status_until_ms;
} desibel_state_t;

static void *desibel_state_ptr;
#define desibel (*(desibel_state_t *)desibel_state_ptr)

/* Ses gorevinin dokundugu tek paylasimli veri; uygulama "durumu" degil,
 * salt senkronizasyon ilkeli oldugu icin dosya kapsaminda tutulur (bkz.
 * solar_os_recorder.c'deki ayni desen). */
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("desibel audio worker spinlock, no app data")
static portMUX_TYPE desibel_lock = portMUX_INITIALIZER_UNLOCKED;

static void desibel_render(solar_os_context_t *ctx);
static void desibel_set_status(const char *message);
static void desibel_stop_worker(void);

/* ---------------------------------------------------------------------
 * Hann penceresi (Q15)
 * ------------------------------------------------------------------- */

static void desibel_build_window(void)
{
    for (size_t i = 0U; i < DESIBEL_FFT_SIZE; i++) {
        const float phase = 2.0f * DESIBEL_PI * (float)i / (float)(DESIBEL_FFT_SIZE - 1U);
        const float w = 0.5f - 0.5f * cosf(phase);
        int32_t q15 = (int32_t)(w * 32767.0f);
        if (q15 > 32767) {
            q15 = 32767;
        }
        if (q15 < 0) {
            q15 = 0;
        }
        desibel.window_q15[i] = (int16_t)q15;
    }
}

/* ---------------------------------------------------------------------
 * Ses gorevi geri cagirimlari (gorev baglaminda calisir - hizli olmali)
 * ------------------------------------------------------------------- */

static bool desibel_cancel_cb(void *user)
{
    (void)user;
    return desibel.stop_requested;
}

static void desibel_samples_cb(const int16_t *samples, size_t sample_count,
                               uint8_t channels, void *user)
{
    (void)user;
    if (sample_count == 0U || channels == 0U) {
        return;
    }
    const size_t frames = sample_count / channels;
    portENTER_CRITICAL(&desibel_lock);
    for (size_t i = 0U; i < frames; i++) {
        const int16_t s = samples[i * channels];
        if (desibel.staging_count < DESIBEL_FFT_SIZE) {
            desibel.staging[desibel.staging_count++] = s;
        }
        desibel.level_sum_sq += (uint64_t)((int32_t)s * (int32_t)s);
        desibel.level_sample_count++;
        const int16_t abs_s = s < 0 ? (int16_t)(-(int32_t)s) : s;
        if (abs_s > desibel.level_peak_abs) {
            desibel.level_peak_abs = abs_s;
        }
    }
    if (desibel.staging_count >= DESIBEL_FFT_SIZE) {
        desibel.staging_ready = true;
    }
    portEXIT_CRITICAL(&desibel_lock);
}

static void desibel_worker(void *arg)
{
    (void)arg;
    solar_os_audio_wav_info_t info = {0};
    const solar_os_audio_wav_options_t options = {
        .owner = "desibel",
        .capture_stream = NULL,
        .record_format =
            {
                .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
                .sample_rate = DESIBEL_SAMPLE_RATE,
                .channels = DESIBEL_CHANNELS,
                .bits_per_sample = 16U,
            },
        .monitor = false,
        .monitor_volume = 0U,
        .should_monitor = NULL,
        .should_cancel = desibel_cancel_cb,
        .progress = NULL,
        .should_pause = NULL,
        .samples = desibel_samples_cb,
        .device = NULL,
        .user = NULL,
        .progress_interval_ms = 250U,
    };
    /* volume=0: sadece analiz, hoparlore gecirme yok. */
    const esp_err_t err = solar_os_audio_monitor_stream(0U, &options, &info);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        SOLAR_OS_LOGW(TAG, "monitor_stream ended: %s", esp_err_to_name(err));
    }
    portENTER_CRITICAL(&desibel_lock);
    desibel.task_done = true;
    portEXIT_CRITICAL(&desibel_lock);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static esp_err_t desibel_start_worker(void)
{
    if (desibel.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    desibel.stop_requested = false;
    desibel.task_done = false;
    const BaseType_t created = solar_os_task_create_pinned_internal(
        desibel_worker, "desibel_monitor", DESIBEL_TASK_STACK, NULL,
        DESIBEL_TASK_PRIORITY, &desibel.task, tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        desibel.task = NULL;
        desibel.task_done = true;
        SOLAR_OS_LOGW(TAG, "worker task creation failed");
        return ESP_ERR_NO_MEM;
    }
    desibel.monitoring = true;
    return ESP_OK;
}

static void desibel_stop_worker(void)
{
    if (desibel.task == NULL) {
        desibel.monitoring = false;
        return;
    }
    desibel.stop_requested = true;
    if (!solar_os_task_wait_done(desibel.task, &desibel.task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        /* should_cancel gormezden gelinip gorev hala calisiyor olabilir;
         * gercekten bitmeden silmek, artik gecersiz durum blogunun uzerine
         * yazmaya devam eden bir gorev birakabilir -- bitene kadar bekle. */
        SOLAR_OS_LOGW(TAG, "worker did not stop within %u ms, waiting for completion",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        while (!desibel.task_done) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    solar_os_task_delete_internal(desibel.task);
    desibel.task = NULL;
    desibel.task_done = true;
    desibel.stop_requested = false;
    desibel.monitoring = false;
}

/* ---------------------------------------------------------------------
 * dB / spektrum hesaplama (ana thread, TICK sirasinda)
 * ------------------------------------------------------------------- */

static float desibel_norm_to_db(float norm)
{
    if (norm < DESIBEL_NORM_FLOOR) {
        norm = DESIBEL_NORM_FLOOR;
    }
    return 20.0f * log10f(norm) + desibel.calibration_offset_db;
}

static void desibel_update_spectrum(uint8_t exponent)
{
    const size_t bins = DESIBEL_FFT_SIZE / 2U;
    const size_t bins_per_band = bins / DESIBEL_BANDS;
    const float scale = (float)(1U << exponent);
    float best_mag = 0.0f;
    size_t best_bin = 0U;

    for (size_t band = 0U; band < DESIBEL_BANDS; band++) {
        const size_t start_bin = band * bins_per_band;
        size_t end_bin = start_bin + bins_per_band;
        if (band == DESIBEL_BANDS - 1U) {
            end_bin = bins;
        }
        float sum = 0.0f;
        for (size_t b = start_bin; b < end_bin; b++) {
            const float re = (float)desibel.spectrum[b].real;
            const float im = (float)desibel.spectrum[b].imag;
            const float mag = sqrtf(re * re + im * im) * scale;
            sum += mag;
            if (b > 0U && mag > best_mag) {
                best_mag = mag;
                best_bin = b;
            }
        }
        const size_t band_bins = end_bin > start_bin ? (end_bin - start_bin) : 1U;
        const float avg_mag = sum / (float)band_bins;
        const float db = desibel_norm_to_db(avg_mag / 32768.0f);
        desibel.band_db[band] = db;
        if (db > desibel.band_peak_db[band]) {
            desibel.band_peak_db[band] = db;
        } else {
            desibel.band_peak_db[band] -= 0.6f;
            if (desibel.band_peak_db[band] < db) {
                desibel.band_peak_db[band] = db;
            }
        }

        int level = (int)((db - DESIBEL_DB_FLOOR) / 6.0f);
        if (level < 0) {
            level = 0;
        }
        if (level > 15) {
            level = 15;
        }
        desibel.spectrogram[desibel.spectrogram_head][band] = (uint8_t)level;
    }

    desibel.spectrogram_head = (desibel.spectrogram_head + 1U) % DESIBEL_HISTORY_COLUMNS;
    if (desibel.spectrogram_count < DESIBEL_HISTORY_COLUMNS) {
        desibel.spectrogram_count++;
    }
    if (best_bin > 0U) {
        desibel.dominant_hz =
            (float)best_bin * (float)DESIBEL_SAMPLE_RATE / (float)DESIBEL_FFT_SIZE;
    }
}

static void desibel_log_write(float db, float peak_db)
{
    if (!desibel.logging || desibel.log_file == NULL) {
        return;
    }
    fprintf(desibel.log_file, "%u,%.1f,%.1f,%.0f\n", (unsigned)desibel.elapsed_ms, db,
            peak_db, desibel.dominant_hz);
    if ((desibel.elapsed_ms - desibel.log_last_flush_ms) > 2000U) {
        fflush(desibel.log_file);
        desibel.log_last_flush_ms = desibel.elapsed_ms;
    }
}

static void desibel_process_tick(void)
{
    int16_t local_samples[DESIBEL_FFT_SIZE];
    bool have_frame = false;
    uint64_t sum_sq = 0U;
    uint32_t count = 0U;
    int16_t peak_abs = 0;

    portENTER_CRITICAL(&desibel_lock);
    if (desibel.staging_ready) {
        memcpy(local_samples, desibel.staging, sizeof(local_samples));
        desibel.staging_count = 0U;
        desibel.staging_ready = false;
        have_frame = true;
    }
    sum_sq = desibel.level_sum_sq;
    count = desibel.level_sample_count;
    peak_abs = desibel.level_peak_abs;
    desibel.level_sum_sq = 0U;
    desibel.level_sample_count = 0U;
    desibel.level_peak_abs = 0;
    portEXIT_CRITICAL(&desibel_lock);

    if (count > 0U) {
        const float rms = sqrtf((float)sum_sq / (float)count);
        const float db = desibel_norm_to_db(rms / 32768.0f);
        const float peak_db = desibel_norm_to_db((float)peak_abs / 32768.0f);
        desibel.current_db = db;
        desibel.peak_db = peak_db;
        if (!desibel.have_session_stats) {
            desibel.min_db_session = db;
            desibel.max_db_session = db;
            desibel.have_session_stats = true;
        } else {
            if (db < desibel.min_db_session) {
                desibel.min_db_session = db;
            }
            if (db > desibel.max_db_session) {
                desibel.max_db_session = db;
            }
        }
        desibel.level_history[desibel.level_head] = db;
        desibel.level_head = (desibel.level_head + 1U) % DESIBEL_TIME_HISTORY;
        if (desibel.level_count < DESIBEL_TIME_HISTORY) {
            desibel.level_count++;
        }
        desibel_log_write(db, peak_db);
        desibel.render_pending = true;
    }

    if (have_frame && desibel.fft != NULL) {
        (void)solar_os_dsp_window_q15(desibel.windowed, local_samples,
                                      desibel.window_q15, DESIBEL_FFT_SIZE);
        uint8_t exponent = 0U;
        const esp_err_t err = solar_os_dsp_fft_execute(desibel.fft, desibel.spectrum,
                                                        desibel.windowed, &exponent);
        if (err == ESP_OK) {
            desibel_update_spectrum(exponent);
            desibel.render_pending = true;
        }
    }
}

/* ---------------------------------------------------------------------
 * Kayit (CSV log)
 * ------------------------------------------------------------------- */

static void desibel_toggle_logging(void)
{
    if (desibel.logging) {
        if (desibel.log_file != NULL) {
            fflush(desibel.log_file);
            fclose(desibel.log_file);
            desibel.log_file = NULL;
        }
        desibel.logging = false;
        desibel_set_status("Kayit durduruldu");
        return;
    }

    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_resolve_path("/" DESIBEL_LOG_DIR, dir, sizeof(dir)) == ESP_OK) {
        (void)solar_os_storage_mkdir(dir);
    }
    if (solar_os_storage_resolve_path("/" DESIBEL_LOG_DIR "/" DESIBEL_LOG_FILE, path,
                                      sizeof(path)) != ESP_OK) {
        desibel_set_status("Log yolu cozumlenemedi");
        return;
    }
    FILE *f = fopen(path, "ab");
    if (f == NULL) {
        desibel_set_status("Log dosyasi acilamadi");
        return;
    }
    if (ftell(f) == 0L) {
        fprintf(f, "ms,db,peak_db,dominant_hz\n");
    }
    desibel.log_file = f;
    desibel.logging = true;
    desibel.log_last_flush_ms = desibel.elapsed_ms;
    desibel_set_status("Kayit basladi: " DESIBEL_LOG_DIR "/" DESIBEL_LOG_FILE);
}

/* ---------------------------------------------------------------------
 * Durum satiri
 * ------------------------------------------------------------------- */

static void desibel_set_status(const char *message)
{
    strncpy(desibel.status_message, message, sizeof(desibel.status_message) - 1U);
    desibel.status_message[sizeof(desibel.status_message) - 1U] = '\0';
    desibel.status_until_ms = desibel.elapsed_ms + 2500U;
    desibel.render_pending = true;
}

/* ---------------------------------------------------------------------
 * Cizim yardimcilari
 * ------------------------------------------------------------------- */

static int desibel_db_to_y(float db, int top, int height)
{
    float clamped = db;
    if (clamped < DESIBEL_DB_FLOOR) {
        clamped = DESIBEL_DB_FLOOR;
    }
    if (clamped > DESIBEL_DB_CEIL) {
        clamped = DESIBEL_DB_CEIL;
    }
    const float norm = (clamped - DESIBEL_DB_FLOOR) / (DESIBEL_DB_CEIL - DESIBEL_DB_FLOOR);
    return top + height - (int)(norm * (float)height);
}

static const char * const DESIBEL_TAB_NAMES[DESIBEL_VIEW_COUNT] = {
    "Overview", "Spectrum", "Spectrogram",
};

static int desibel_body_top(solar_os_gfx_t *gfx)
{
    return solar_os_appbar_header_height(gfx) + solar_os_appbar_status_line_height(gfx) + 4;
}

static void desibel_draw_header(solar_os_gfx_t *gfx)
{
    char status[64];
    snprintf(status, sizeof(status), "Cal: %+.0f dB | %s",
             (double)desibel.calibration_offset_db,
             desibel.monitoring ? "LISTENING" : "PAUSED");

    const solar_os_appbar_header_t header = {
        .title = "Decibel Meter",
        .show_back = true,
        .tabs = {
            .names = DESIBEL_TAB_NAMES,
            .count = DESIBEL_VIEW_COUNT,
            .active_index = (size_t)desibel.view,
        },
        .status_line = status,
    };
    solar_os_appbar_draw_header(gfx, &header);
}

static void desibel_draw_footer(solar_os_gfx_t *gfx)
{
    if (desibel.status_until_ms > desibel.elapsed_ms && desibel.status_message[0] != '\0') {
        const int width = (int)solar_os_gfx_width(gfx);
        const int height = (int)solar_os_gfx_height(gfx);
        const int footer_h = solar_os_appbar_footer_height(gfx);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_rect(gfx, 0, height - footer_h, width, footer_h);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 8, height - footer_h / 4, desibel.status_message);
        return;
    }

    solar_os_appbar_shortcut_t items[2];
    items[0].key = ' ';
    items[0].ctrl = false;
    snprintf(items[0].label, sizeof(items[0].label), "%s", desibel.monitoring ? "Pause" : "Resume");
    items[1].key = 'L';
    items[1].ctrl = true;
    snprintf(items[1].label, sizeof(items[1].label), "%s", desibel.logging ? "Log Off" : "Log On");

    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = 2 };
    solar_os_appbar_draw_footer(gfx, &shortcuts);
}

static void desibel_draw_overview(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = desibel_body_top(gfx);
    const int bottom = height - solar_os_appbar_footer_height(gfx) - 4;

    /* Big numeric reading */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    char big[32];
    snprintf(big, sizeof(big), "%.1f dBFS", (double)desibel.current_db);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, 10, top + 22, big);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char stats[110];
    snprintf(stats, sizeof(stats), "Peak: %.1f dB   Min: %.1f   Max: %.1f   Dominant: %.0f Hz",
             (double)desibel.peak_db, (double)desibel.min_db_session,
             (double)desibel.max_db_session, (double)desibel.dominant_hz);
    solar_os_gfx_text(gfx, 10, top + 38, stats);

    /* Horizontal level meter */
    const int bar_x = 10;
    const int bar_y = top + 46;
    const int bar_w = width - 20;
    const int bar_h = 14;
    solar_os_gfx_rect(gfx, bar_x, bar_y, bar_w, bar_h);
    float norm = (desibel.current_db - DESIBEL_DB_FLOOR) / (DESIBEL_DB_CEIL - DESIBEL_DB_FLOOR);
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    const int fill_w = (int)((float)(bar_w - 4) * norm);
    if (fill_w > 0) {
        solar_os_gfx_fill_rect(gfx, bar_x + 2, bar_y + 2, fill_w, bar_h - 4);
    }

    /* History line chart */
    const int graph_top = bar_y + bar_h + 10;
    const int graph_h = bottom - graph_top;
    if (graph_h > 10 && desibel.level_count > 1U) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_line(gfx, 10, graph_top, width - 10, graph_top);
        solar_os_gfx_line(gfx, 10, graph_top + graph_h / 2, width - 10, graph_top + graph_h / 2);
        solar_os_gfx_line(gfx, 10, bottom, width - 10, bottom);

        const int usable_w = width - 20;
        const size_t n = desibel.level_count;
        const size_t oldest = (desibel.level_head + DESIBEL_TIME_HISTORY - n) % DESIBEL_TIME_HISTORY;
        int prev_x = 10;
        int prev_y = desibel_db_to_y(desibel.level_history[oldest], graph_top, graph_h);
        for (size_t i = 1U; i < n; i++) {
            const size_t idx = (oldest + i) % DESIBEL_TIME_HISTORY;
            const int x = 10 + (int)((size_t)usable_w * i / (n - 1U));
            const int y = desibel_db_to_y(desibel.level_history[idx], graph_top, graph_h);
            solar_os_gfx_line(gfx, prev_x, prev_y, x, y);
            prev_x = x;
            prev_y = y;
        }
    }
}

static void desibel_draw_spectrum(solar_os_gfx_t *gfx, int width, int height)
{
    const int label_y = desibel_body_top(gfx) + 10;
    const int top = desibel_body_top(gfx) + 26;
    const int bottom = height - solar_os_appbar_footer_height(gfx) - 4;
    const int area_h = bottom - top;
    if (area_h <= 0) return;

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    char label[64];
    snprintf(label, sizeof(label), "Audio Spectrum (0 - %u Hz) | Dominant: %.0f Hz",
             (unsigned)(DESIBEL_SAMPLE_RATE / 2U), (double)desibel.dominant_hz);
    solar_os_gfx_text(gfx, 8, label_y, label);

    const int margin = 8;
    const int usable_w = width - 2 * margin;
    const int bar_gap = 2;
    const int bar_w = (usable_w / (int)DESIBEL_BANDS) - bar_gap;
    if (bar_w < 1) return;

    for (size_t band = 0U; band < DESIBEL_BANDS; band++) {
        const int x = margin + (int)band * (bar_w + bar_gap);
        const int y = desibel_db_to_y(desibel.band_db[band], top, area_h);
        const int h = (top + area_h) - y;
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
        if (h > 0) {
            solar_os_gfx_fill_rect(gfx, x, y, bar_w, h);
        }
        const int peak_y = desibel_db_to_y(desibel.band_peak_db[band], top, area_h);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_line(gfx, x, peak_y, x + bar_w, peak_y);
    }
    solar_os_gfx_rect(gfx, margin, top, usable_w, area_h);
}

static void desibel_draw_spectrogram(solar_os_gfx_t *gfx, int width, int height)
{
    const int top = desibel_body_top(gfx);
    const int bottom = height - solar_os_appbar_footer_height(gfx) - 4;
    const int area_h = bottom - top;
    const int area_w = width - 8;
    if (area_h <= 0 || area_w <= 0 || desibel.spectrogram_count == 0U) return;

    const int col_w = area_w / (int)DESIBEL_HISTORY_COLUMNS;
    const int row_h = area_h / (int)DESIBEL_BANDS;
    if (col_w < 1 || row_h < 1) return;

    for (size_t c = 0U; c < desibel.spectrogram_count; c++) {
        const size_t buf_index = (desibel.spectrogram_head + DESIBEL_HISTORY_COLUMNS -
                                  desibel.spectrogram_count + c) % DESIBEL_HISTORY_COLUMNS;
        const int x = 4 + (int)c * col_w;
        for (size_t band = 0U; band < DESIBEL_BANDS; band++) {
            const uint8_t level = desibel.spectrogram[buf_index][band];
            const int y = top + area_h - ((int)band + 1) * row_h;
            solar_os_gfx_set_color(gfx, solar_os_gfx_gray(15U - level));
            solar_os_gfx_fill_rect(gfx, x, y, col_w, row_h);
        }
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 4, top, area_w, area_h);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 6, top + 12, "High Freq (4 kHz)");
    solar_os_gfx_text(gfx, 6, bottom - 2, "Low Freq (0 Hz) -> Time Scrolling");
}

static void desibel_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    desibel_draw_header(gfx);

    switch (desibel.view) {
    case DESIBEL_VIEW_SPECTRUM:
        desibel_draw_spectrum(gfx, width, height);
        break;
    case DESIBEL_VIEW_SPECTROGRAM:
        desibel_draw_spectrogram(gfx, width, height);
        break;
    case DESIBEL_VIEW_OVERVIEW:
    default:
        desibel_draw_overview(gfx, width, height);
        break;
    }

    desibel_draw_footer(gfx);
    solar_os_gfx_present(gfx);
    desibel.render_pending = false;
}

/* ---------------------------------------------------------------------
 * Olay isleme
 * ------------------------------------------------------------------- */

static void desibel_handle_char(solar_os_context_t *ctx, char ch)
{
    const unsigned char uch = (unsigned char)ch;

    if (uch == SOLAR_OS_KEY_ESCAPE) {
        if (desibel.logging && desibel.log_file != NULL) {
            fflush(desibel.log_file);
            fclose(desibel.log_file);
            desibel.log_file = NULL;
        }
        desibel_stop_worker();
        solar_os_context_request_exit(ctx);
        return;
    }
    if (ch == '\t') {
        desibel.view = (desibel_view_t)((desibel.view + 1) % DESIBEL_VIEW_COUNT);
        desibel.render_pending = true;
        return;
    }
    if (ch == '1') {
        desibel.view = DESIBEL_VIEW_OVERVIEW;
        desibel.render_pending = true;
        return;
    }
    if (ch == '2') {
        desibel.view = DESIBEL_VIEW_SPECTRUM;
        desibel.render_pending = true;
        return;
    }
    if (ch == '3') {
        desibel.view = DESIBEL_VIEW_SPECTROGRAM;
        desibel.render_pending = true;
        return;
    }
    if (ch == ' ') {
        if (desibel.monitoring) {
            desibel_stop_worker();
            desibel_set_status("Dinleme duraklatildi");
        } else {
            if (desibel_start_worker() == ESP_OK) {
                desibel_set_status("Dinleme basladi");
            } else {
                desibel_set_status("Mikrofon baslatilamadi");
            }
        }
        return;
    }
    if (ch == 'l' || ch == 'L') {
        desibel_toggle_logging();
        return;
    }
    if (ch == '+' || ch == '=') {
        desibel.calibration_offset_db += 1.0f;
        desibel.render_pending = true;
        return;
    }
    if (ch == '-' || ch == '_') {
        desibel.calibration_offset_db -= 1.0f;
        desibel.render_pending = true;
        return;
    }
    if (ch == 'r' || ch == 'R') {
        desibel.have_session_stats = false;
        desibel_set_status("Oturum istatistikleri sifirlandi");
        return;
    }
}

static bool desibel_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    switch (event->type) {
    case SOLAR_OS_EVENT_CHAR:
        desibel_handle_char(ctx, event->data.ch);
        break;
    case SOLAR_OS_EVENT_TICK:
        desibel.elapsed_ms += DESIBEL_TICK_MS;
        desibel_process_tick();
        if (desibel.status_until_ms != 0U && desibel.status_until_ms <= desibel.elapsed_ms &&
            desibel.status_until_ms + DESIBEL_TICK_MS > desibel.elapsed_ms) {
            desibel.render_pending = true;
        }
        if (desibel.render_pending) {
            desibel_render(ctx);
        }
        break;
    case SOLAR_OS_EVENT_RESUME:
        desibel.render_pending = true;
        desibel_render(ctx);
        break;
    case SOLAR_OS_EVENT_CLICK: {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) break;

        const solar_os_appbar_header_t header = {
            .title = "Decibel Meter",
            .show_back = true,
            .tabs = {
                .names = DESIBEL_TAB_NAMES,
                .count = DESIBEL_VIEW_COUNT,
                .active_index = (size_t)desibel.view,
            },
        };
        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, event->data.click.x, event->data.click.y, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                desibel_handle_char(ctx, (char)SOLAR_OS_KEY_ESCAPE);
            } else if (hit.kind == SOLAR_OS_APPBAR_HIT_TAB_ITEM && hit.index < DESIBEL_VIEW_COUNT) {
                desibel.view = (desibel_view_t)hit.index;
                desibel.render_pending = true;
            }
            break;
        }

        const bool showing_status = desibel.status_until_ms > desibel.elapsed_ms && desibel.status_message[0] != '\0';
        if (!showing_status) {
            solar_os_appbar_shortcut_t items[2];
            items[0].key = ' ';
            items[0].ctrl = false;
            snprintf(items[0].label, sizeof(items[0].label), "%s", desibel.monitoring ? "Pause" : "Resume");
            items[1].key = 'L';
            items[1].ctrl = true;
            snprintf(items[1].label, sizeof(items[1].label), "%s", desibel.logging ? "Log Off" : "Log On");
            const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = 2 };

            solar_os_appbar_hit_t fhit;
            if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, event->data.click.x, event->data.click.y, &fhit) &&
                fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM) {
                if (fhit.index == 0) {
                    desibel_handle_char(ctx, ' ');
                } else if (fhit.index == 1) {
                    desibel_handle_char(ctx, 'L');
                }
            }
        }
        if (desibel.render_pending) {
            desibel_render(ctx);
        }
        break;
    }
    default:
        break;
    }
    return true;
}

/* ---------------------------------------------------------------------
 * Yasam dongusu
 * ------------------------------------------------------------------- */

static esp_err_t desibel_start(solar_os_context_t *ctx)
{
    memset(&desibel, 0, sizeof(desibel));
    desibel.calibration_offset_db = 0.0f;
    desibel.view = DESIBEL_VIEW_OVERVIEW;
    desibel.render_pending = true;

    desibel_build_window();
    const esp_err_t fft_err = solar_os_dsp_fft_create(DESIBEL_FFT_SIZE, &desibel.fft);
    if (fft_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "fft create failed: %s", esp_err_to_name(fft_err));
    }

    solar_os_context_set_graphics_active(ctx, true);

    const esp_err_t worker_err = desibel_start_worker();
    if (worker_err != ESP_OK) {
        desibel_set_status("Mikrofon baslatilamadi");
    }

    desibel_render(ctx);
    return ESP_OK;
}

static void desibel_suspend(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
    desibel_stop_worker();
}

static void desibel_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    if (!desibel.monitoring) {
        (void)desibel_start_worker();
    }
    desibel.render_pending = true;
    desibel_render(ctx);
}

static void desibel_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    desibel_stop_worker();
    if (desibel.log_file != NULL) {
        fflush(desibel.log_file);
        fclose(desibel.log_file);
        desibel.log_file = NULL;
    }
    if (desibel.fft != NULL) {
        solar_os_dsp_fft_destroy(desibel.fft);
        desibel.fft = NULL;
    }
}

static void desibel_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    snprintf(buffer, buffer_len, "Desibel: %.1f dB", (double)desibel.current_db);
}

const solar_os_app_t solar_os_desibel_app = {
    .name = "desibel",
    .summary = "canli desibel / spektrum / spektrogram olcer",
    .flags = 0,
    .start = desibel_start,
    .suspend = desibel_suspend,
    .resume = desibel_resume,
    .stop = desibel_stop,
    .event = desibel_event,
    .title = desibel_title,
    .state_slot = &desibel_state_ptr,
    .state_size = sizeof(desibel_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = DESIBEL_TASK_STACK,
    .worker_stack_external = false,
    .tick_interval_ms = DESIBEL_TICK_MS,
    .tick_deadline_ms = DESIBEL_TICK_MS * 2U,
    .requested_tick_interval_ms = NULL,
};
