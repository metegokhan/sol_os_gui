/*
 * Mors - otomatik (mikrofon/desibel tabanli) mors kodu alici ve
 * ekran+ses ile gonderici (SolarOS native uygulamasi)
 *
 * Bu dosya SolarOS kaynak agacinin DISINDA gelistirilmektedir
 * (bkz. ../../docs/solaros_native_app_notes.md ve ../../integration/README.md).
 * Ses gorevi deseni solar_os_desibel.c ile, dosya kaydetme deseni
 * solar_os_yazici.c ile ayni saglam, kaynak-dogrulanmis API'lere dayanir.
 *
 * Alici, mikrofon RMS seviyesini surekli izler; kendiliginden uyarlanan
 * (adaptive) bir "birim sure" (dit) tahmini ile nokta/cizgi ve harf/kelime
 * bosluklarini ayirt eder -- elle "dinlemeye basla" tuslamaya gerek yoktur,
 * uygulama acilir acilmaz dinler (istenirse Bosluk ile duraklatilabilir).
 */

#include "solar_os_mors.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "solar_os_audio.h"
#include "solar_os_appbar.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_storage.h"
#include "solar_os_stream.h"
#include "solar_os_task.h"

#define TAG "mors"

#define MORSE_SAMPLE_RATE 8000U
#define MORSE_TASK_STACK 6144U
#define MORSE_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define MORSE_TICK_MS 20U

#define MORSE_COMPOSE_MAX 121U
#define MORSE_RX_TEXT_MAX 240U
#define MORSE_RX_HISTORY_LINES 3U
#define MORSE_ELEMENTS_MAX 700U
#define MORSE_SEND_TONE_HZ 700U
#define MORSE_SYMBOL_MAX 8U

#define MORSE_LOG_DIR "mors"
#define MORSE_LOG_FILE "gunluk.txt"
#define MORSE_LOG_ENTRY_MAX 96U
#define MORSE_LOG_ENTRIES_MAX 40U

#define MORSE_HEADER_H 22
#define MORSE_FOOTER_H 18

typedef enum {
    MORSE_VIEW_SEND = 0,
    MORSE_VIEW_RECEIVE,
    MORSE_VIEW_LOG,
    MORSE_VIEW_COUNT,
} morse_view_t;

typedef struct {
    bool mark;
    uint32_t duration_ms;
    char display_char;   /* kaynak karakter (yalniz mark ogeleri icin anlamli) */
    const char *code;    /* bu karakterin tam mors kodu (MORSE_TABLE'a isaretci) */
    size_t symbol_index;  /* code[] icinde bu ogenin karsiladigi nokta/cizgi konumu */
} morse_element_t;

typedef struct {
    char text[MORSE_LOG_ENTRY_MAX];
    bool is_tx;
} morse_log_entry_t;

typedef struct {
    /* --- gorunum --- */
    morse_view_t view;
    uint32_t elapsed_ms;
    bool render_pending;
    char status_message[64];
    uint32_t status_until_ms;

    /* --- gonderme --- */
    char compose[MORSE_COMPOSE_MAX];
    size_t compose_len;
    uint32_t wpm;

    morse_element_t send_elements[MORSE_ELEMENTS_MAX];
    size_t send_element_count;
    size_t send_element_index;   /* ana thread'in gorduğu, senkronize edilmis konum */
    bool sending;
    bool send_visual_on;         /* true = bu an ton cikiyor (dolu cizim), false = bosluk (anahat) */

    /* --- gonderici ses gorevi: send_elements'i tek tek calar.
     * solar_os_audio_tone_enqueue() tek istekte en fazla
     * SOLAR_OS_AUDIO_TONE_SEQUENCE_MAX_STEPS (8) adim kabul ediyor --
     * gercek bir mesaj icin cok az -- bu yuzden her ogeyi ayri ayri
     * bloklayan tek-ton API'siyle calan bir gorev kullaniliyor. Gorev,
     * su an hangi ogeyi caldigini tx_current_index'e yazar (mors_lock
     * ile korunan), boylece ekran gorseli gercekte calan sesle birebir
     * senkron kalir (ayri, tahmine dayali bir zamanlayici yerine). --- */
    TaskHandle_t tx_task;
    volatile bool tx_task_done;
    volatile bool tx_stop_requested;
    size_t tx_current_index;

    /* --- alma: ses gorevi (mors_lock ile korunan paylasimli alanlar) --- */
    TaskHandle_t task;
    volatile bool task_done;
    volatile bool stop_requested;
    bool listening;
    uint64_t rx_sum_sq;
    uint32_t rx_sample_count;

    /* --- alma: kod-cozme durum makinesi (yalniz ana thread) --- */
    bool rx_mark_active;
    uint32_t rx_state_ms;
    uint32_t rx_dit_ms;
    float rx_noise_floor;
    float rx_level;
    bool rx_word_space_inserted; /* bu sessizlik donemi icin kelime bosluğu zaten eklendi mi */
    bool rx_message_flushed;     /* bu sessizlik donemi icin mesaj zaten gunluge yazildi mi */
    char rx_symbol_buf[MORSE_SYMBOL_MAX];
    size_t rx_symbol_len;
    char rx_text[MORSE_RX_TEXT_MAX];
    size_t rx_text_len;
    char rx_history[MORSE_RX_HISTORY_LINES][MORSE_RX_TEXT_MAX];

    /* --- gunluk (kalici, dosyaya yaziliyor) --- */
    FILE *log_file;
    uint32_t log_last_flush_ms;
    bool log_loaded;
    morse_log_entry_t log_entries[MORSE_LOG_ENTRIES_MAX];
    size_t log_entry_count;
    size_t log_selected;
} morse_state_t;

static void *morse_state_ptr;
#define morse (*(morse_state_t *)morse_state_ptr)

SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("mors audio worker spinlock, no app data")
static portMUX_TYPE morse_lock = portMUX_INITIALIZER_UNLOCKED;

static void morse_render(solar_os_context_t *ctx);
static void morse_set_status(const char *message);
static void morse_stop_worker(void);
static void morse_log_write(bool is_tx, const char *text);

/* ---------------------------------------------------------------------
 * Mors kodu tablosu
 * ------------------------------------------------------------------- */

typedef struct {
    char ch;
    const char *code;
} morse_table_entry_t;

static const morse_table_entry_t MORSE_TABLE[] = {
    {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},   {'E', "."},
    {'F', "..-."},  {'G', "--."},   {'H', "...."},  {'I', ".."},    {'J', ".---"},
    {'K', "-.-"},   {'L', ".-.."},  {'M', "--"},    {'N', "-."},    {'O', "---"},
    {'P', ".--."},  {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
    {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},  {'Y', "-.--"},
    {'Z', "--.."},
    {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"}, {'4', "....-"},
    {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},
};
#define MORSE_TABLE_COUNT (sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]))

static const char *morse_encode_char(char c)
{
    const char upper = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    for (size_t i = 0U; i < MORSE_TABLE_COUNT; i++) {
        if (MORSE_TABLE[i].ch == upper) {
            return MORSE_TABLE[i].code;
        }
    }
    return NULL;
}

static char morse_decode_symbol(const char *code)
{
    for (size_t i = 0U; i < MORSE_TABLE_COUNT; i++) {
        if (strcmp(MORSE_TABLE[i].code, code) == 0) {
            return MORSE_TABLE[i].ch;
        }
    }
    return '?';
}

/* ---------------------------------------------------------------------
 * Ses gorevi (alma - dinleme)
 * ------------------------------------------------------------------- */

static bool morse_cancel_cb(void *user)
{
    (void)user;
    return morse.stop_requested;
}

static void morse_samples_cb(const int16_t *samples, size_t sample_count,
                             uint8_t channels, void *user)
{
    (void)user;
    if (sample_count == 0U || channels == 0U) {
        return;
    }
    const size_t frames = sample_count / channels;
    portENTER_CRITICAL(&morse_lock);
    for (size_t i = 0U; i < frames; i++) {
        const int16_t s = samples[i * channels];
        morse.rx_sum_sq += (uint64_t)((int32_t)s * (int32_t)s);
        morse.rx_sample_count++;
    }
    portEXIT_CRITICAL(&morse_lock);
}

static void morse_worker(void *arg)
{
    (void)arg;
    solar_os_audio_wav_info_t info = {0};
    const solar_os_audio_wav_options_t options = {
        .owner = "mors",
        .capture_stream = NULL,
        .record_format =
            {
                .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
                .sample_rate = MORSE_SAMPLE_RATE,
                .channels = 1U,
                .bits_per_sample = 16U,
            },
        .monitor = false,
        .monitor_volume = 0U,
        .should_monitor = NULL,
        .should_cancel = morse_cancel_cb,
        .progress = NULL,
        .should_pause = NULL,
        .samples = morse_samples_cb,
        .device = NULL,
        .user = NULL,
        .progress_interval_ms = 250U,
    };
    const esp_err_t err = solar_os_audio_monitor_stream(0U, &options, &info);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        SOLAR_OS_LOGW(TAG, "monitor_stream ended: %s", esp_err_to_name(err));
    }
    portENTER_CRITICAL(&morse_lock);
    morse.task_done = true;
    portEXIT_CRITICAL(&morse_lock);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static esp_err_t morse_start_worker(void)
{
    if (morse.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    morse.stop_requested = false;
    morse.task_done = false;
    const BaseType_t created = solar_os_task_create_pinned_internal(
        morse_worker, "mors_listen", MORSE_TASK_STACK, NULL, MORSE_TASK_PRIORITY,
        &morse.task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        morse.task = NULL;
        morse.task_done = true;
        SOLAR_OS_LOGW(TAG, "worker task creation failed");
        return ESP_ERR_NO_MEM;
    }
    morse.listening = true;
    return ESP_OK;
}

static void morse_stop_worker(void)
{
    if (morse.task == NULL) {
        morse.listening = false;
        return;
    }
    morse.stop_requested = true;
    if (!solar_os_task_wait_done(morse.task, &morse.task_done, SOLAR_OS_TASK_STOP_WAIT_MS)) {
        /* gercekten bitmeden silmek, artik gecersiz durum blogunun uzerine
         * yazmaya devam eden bir gorev birakabilir -- bitene kadar bekle. */
        SOLAR_OS_LOGW(TAG, "worker did not stop within %u ms, waiting for completion",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        while (!morse.task_done) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    solar_os_task_delete_internal(morse.task);
    morse.task = NULL;
    morse.task_done = true;
    morse.stop_requested = false;
    morse.listening = false;
}

/* ---------------------------------------------------------------------
 * Alma: kod cozme durum makinesi
 * ------------------------------------------------------------------- */

static void morse_rx_append_char(char c)
{
    if (morse.rx_text_len + 1U >= MORSE_RX_TEXT_MAX) {
        /* satir dolarsa gecmise kaydir ve devam et */
        memmove(&morse.rx_history[0], &morse.rx_history[1],
                sizeof(morse.rx_history) - sizeof(morse.rx_history[0]));
        strncpy(morse.rx_history[MORSE_RX_HISTORY_LINES - 1U], morse.rx_text,
                MORSE_RX_TEXT_MAX - 1U);
        morse.rx_text_len = 0U;
        morse.rx_text[0] = '\0';
    }
    morse.rx_text[morse.rx_text_len++] = c;
    morse.rx_text[morse.rx_text_len] = '\0';
}

static void morse_finish_rx_message(void)
{
    if (morse.rx_text_len == 0U) {
        return;
    }
    morse_log_write(false, morse.rx_text);
    memmove(&morse.rx_history[0], &morse.rx_history[1],
            sizeof(morse.rx_history) - sizeof(morse.rx_history[0]));
    strncpy(morse.rx_history[MORSE_RX_HISTORY_LINES - 1U], morse.rx_text,
            MORSE_RX_TEXT_MAX - 1U);
    morse.rx_text_len = 0U;
    morse.rx_text[0] = '\0';
    morse_set_status("Message logged");
}

static void morse_process_mark(uint32_t duration_ms)
{
    if (morse.rx_dit_ms == 0U) {
        morse.rx_dit_ms = duration_ms; /* ilk isaretle birim suresini baslat */
    }
    const bool is_dash = duration_ms > morse.rx_dit_ms * 2U;
    if (morse.rx_symbol_len < MORSE_SYMBOL_MAX - 1U) {
        morse.rx_symbol_buf[morse.rx_symbol_len++] = is_dash ? '-' : '.';
        morse.rx_symbol_buf[morse.rx_symbol_len] = '\0';
    }
    const float target = is_dash ? (float)duration_ms / 3.0f : (float)duration_ms;
    float adapted = (float)morse.rx_dit_ms * 0.7f + target * 0.3f;
    if (adapted < 20.0f) {
        adapted = 20.0f;
    }
    morse.rx_dit_ms = (uint32_t)adapted;
}

static void morse_commit_symbol(void)
{
    if (morse.rx_symbol_len == 0U) {
        return;
    }
    const char decoded = morse_decode_symbol(morse.rx_symbol_buf);
    morse_rx_append_char(decoded);
    morse.rx_symbol_len = 0U;
    morse.rx_symbol_buf[0] = '\0';
    morse.render_pending = true;
}

static void morse_decode_tick(void)
{
    uint64_t sum_sq;
    uint32_t count;
    portENTER_CRITICAL(&morse_lock);
    sum_sq = morse.rx_sum_sq;
    count = morse.rx_sample_count;
    morse.rx_sum_sq = 0U;
    morse.rx_sample_count = 0U;
    portEXIT_CRITICAL(&morse_lock);

    if (count == 0U) {
        return;
    }

    const float rms = sqrtf((float)sum_sq / (float)count);
    morse.rx_level = rms;

    if (morse.rx_noise_floor <= 0.0f) {
        morse.rx_noise_floor = rms;
    }
    const float threshold = morse.rx_noise_floor * 2.5f + 300.0f;
    const bool is_mark = rms > threshold;

    if (!is_mark) {
        morse.rx_noise_floor = morse.rx_noise_floor * 0.98f + rms * 0.02f;
    }

    if (is_mark == morse.rx_mark_active) {
        morse.rx_state_ms += MORSE_TICK_MS;

        if (!is_mark) {
            /* Sessizlik surdukce harf/kelime/mesaj bitisini PROAKTIF olarak
             * tamamla -- bir sonraki tonun baslamasini beklemeden. Eskiden
             * bekleyen sembol (rx_symbol_buf) sadece YENI bir ton
             * baslayinca islenirdi: mesajin son harfi hicbir zaman
             * tamamlanmaz (sinyal hic gelmezse), ya da 3 saniyelik uzun
             * sessizlik mesaji satira aktarirken bekleyen harfi
             * atlar -- o harf ancak cok sonra yeni bir ton gelince, artik
             * BOS olan yeni satirin basina eklenirdi. */
            uint32_t letter_gap_ms = morse.rx_dit_ms * 2U;
            if (letter_gap_ms < 100U) {
                letter_gap_ms = 100U;
            }
            if (morse.rx_symbol_len > 0U && morse.rx_state_ms >= letter_gap_ms) {
                morse_commit_symbol();
            }

            uint32_t word_gap_ms = morse.rx_dit_ms * 5U;
            if (word_gap_ms < 300U) {
                word_gap_ms = 300U;
            }
            if (!morse.rx_word_space_inserted && morse.rx_state_ms >= word_gap_ms) {
                if (morse.rx_text_len > 0U && morse.rx_text[morse.rx_text_len - 1U] != ' ') {
                    morse_rx_append_char(' ');
                }
                morse.rx_word_space_inserted = true;
            }

            if (!morse.rx_message_flushed && morse.rx_state_ms >= 3000U) {
                if (morse.rx_text_len > 0U) {
                    morse_finish_rx_message();
                }
                morse.rx_message_flushed = true;
                morse.render_pending = true;
            }
        }
        return;
    }

    const uint32_t duration = morse.rx_state_ms;
    const bool was_mark = morse.rx_mark_active;
    morse.rx_mark_active = is_mark;
    morse.rx_state_ms = MORSE_TICK_MS;

    if (was_mark) {
        if (duration >= MORSE_TICK_MS * 2U) {
            morse_process_mark(duration);
        }
    } else {
        /* yeni bir ton basladi: onceki sessizlik donemi bitti, bu
         * donemin bayraklarini yeni sessizlik donemi icin sifirla */
        morse.rx_word_space_inserted = false;
        morse.rx_message_flushed = false;
    }
    morse.render_pending = true;
}

/* ---------------------------------------------------------------------
 * Gonderme: metinden zamanlama dizisi olusturma
 * ------------------------------------------------------------------- */

static void morse_build_send_sequence(void)
{
    morse.send_element_count = 0U;
    const uint32_t dit = 1200U / (morse.wpm == 0U ? 15U : morse.wpm);
    bool first_symbol_in_message = true;

    for (size_t i = 0U; i < morse.compose_len && morse.send_element_count + 2U < MORSE_ELEMENTS_MAX;
         i++) {
        const char c = morse.compose[i];
        if (c == ' ') {
            if (morse.send_element_count > 0U) {
                morse.send_elements[morse.send_element_count - 1U].duration_ms = dit * 7U;
            }
            first_symbol_in_message = true;
            continue;
        }
        const char *code = morse_encode_char(c);
        if (code == NULL) {
            continue;
        }
        if (!first_symbol_in_message && morse.send_element_count > 0U) {
            morse.send_elements[morse.send_element_count - 1U].duration_ms = dit * 3U;
        }
        first_symbol_in_message = false;
        size_t symbol_index = 0U;
        for (const char *p = code; *p != '\0' &&
                                   morse.send_element_count + 2U < MORSE_ELEMENTS_MAX;
             p++, symbol_index++) {
            const uint32_t mark_len = (*p == '-') ? dit * 3U : dit;
            morse.send_elements[morse.send_element_count].mark = true;
            morse.send_elements[morse.send_element_count].duration_ms = mark_len;
            morse.send_elements[morse.send_element_count].display_char = c;
            morse.send_elements[morse.send_element_count].code = code;
            morse.send_elements[morse.send_element_count].symbol_index = symbol_index;
            morse.send_element_count++;
            morse.send_elements[morse.send_element_count].mark = false;
            morse.send_elements[morse.send_element_count].duration_ms = dit; /* sembol arasi */
            morse.send_elements[morse.send_element_count].display_char = c;
            morse.send_elements[morse.send_element_count].code = code;
            morse.send_elements[morse.send_element_count].symbol_index = symbol_index;
            morse.send_element_count++;
        }
    }
}

/* solar_os_audio_tone_enqueue() tek istekte en fazla
 * SOLAR_OS_AUDIO_TONE_SEQUENCE_MAX_STEPS (8) adim kabul ediyor (bkz.
 * solar_os_audio.c, audio_tone_request_valid) -- neredeyse her gercek
 * mors mesaji bunu asiyor ve istek sessizce ESP_ERR_INVALID_ARG ile
 * reddediliyordu, hic ses cikmiyordu. Bunun yerine gonderim,
 * solar_os_clock.c'nin alarm sesiyle ayni desende, her ogeyi tek tek
 * bloklayan solar_os_audio_play_tone() ile calan bir arka plan
 * gorevinde yapiliyor. */
static void morse_tx_worker(void *arg)
{
    (void)arg;
    for (size_t i = 0U; i < morse.send_element_count && !morse.tx_stop_requested; i++) {
        portENTER_CRITICAL(&morse_lock);
        morse.tx_current_index = i;
        portEXIT_CRITICAL(&morse_lock);

        const uint32_t duration_ms = morse.send_elements[i].duration_ms;
        if (morse.send_elements[i].mark) {
            (void)solar_os_audio_play_tone(MORSE_SEND_TONE_HZ, duration_ms,
                                           SOLAR_OS_AUDIO_VOLUME_GLOBAL);
        } else {
            vTaskDelay(pdMS_TO_TICKS(duration_ms));
        }
    }
    portENTER_CRITICAL(&morse_lock);
    morse.tx_task_done = true;
    portEXIT_CRITICAL(&morse_lock);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static esp_err_t morse_start_tx_worker(void)
{
    morse.tx_stop_requested = false;
    morse.tx_task_done = false;
    const BaseType_t created = solar_os_task_create_pinned_internal(
        morse_tx_worker, "morse_tx", MORSE_TASK_STACK, NULL, MORSE_TASK_PRIORITY,
        &morse.tx_task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        morse.tx_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void morse_stop_tx_worker(void)
{
    if (morse.tx_task == NULL) {
        return;
    }
    morse.tx_stop_requested = true;
    if (!solar_os_task_wait_done(morse.tx_task, &morse.tx_task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        /* solar_os_audio_play_tone() bir ton calarken iptal edilemez;
         * gercekten bitene kadar bekle, aksi halde artik gecersiz olan
         * durum blogunun uzerine yazmaya devam eden bir gorev kalabilir. */
        while (!morse.tx_task_done) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    solar_os_task_delete_internal(morse.tx_task);
    morse.tx_task = NULL;
    morse.tx_stop_requested = false;
}

static void morse_reap_tx_worker(void)
{
    if (morse.tx_task == NULL || !morse.tx_task_done) {
        return;
    }
    solar_os_task_delete_internal(morse.tx_task);
    morse.tx_task = NULL;
    morse.sending = false;
    morse.send_visual_on = false;
    morse_set_status("Transmission complete");
}

static void morse_start_send(void)
{
    if (morse.compose_len == 0U || morse.sending) {
        return;
    }
    morse_build_send_sequence();
    if (morse.send_element_count == 0U) {
        morse_set_status("No characters to encode");
        return;
    }
    morse_stop_worker(); /* hoparloru kullanmadan once mikrofonu birak */
    if (morse_start_tx_worker() != ESP_OK) {
        morse_set_status("Could not play audio");
        return;
    }
    morse.send_element_index = 0U;
    morse.tx_current_index = 0U;
    morse.send_visual_on = morse.send_elements[0].mark;
    morse.sending = true;
    morse_log_write(true, morse.compose);
    morse_set_status("Sending...");
}

/* Ekran gorselini, gonderici gorevinin gercekte hangi ogeyi caldigini
 * bildiren tx_current_index'ten okuyarak senkronlar -- ayri, tahmine
 * dayali bir geri sayim yerine gercek ses ilerlemesini yansitir. */
static void morse_sync_send_display(void)
{
    if (!morse.sending) {
        return;
    }
    size_t idx;
    portENTER_CRITICAL(&morse_lock);
    idx = morse.tx_current_index;
    portEXIT_CRITICAL(&morse_lock);

    if (idx >= morse.send_element_count) {
        return;
    }
    const bool visual_on = morse.send_elements[idx].mark;
    if (idx != morse.send_element_index || visual_on != morse.send_visual_on) {
        morse.send_element_index = idx;
        morse.send_visual_on = visual_on;
        morse.render_pending = true;
    }
}

/* ---------------------------------------------------------------------
 * Gunluk (kalici dosya)
 * ------------------------------------------------------------------- */

static void morse_log_write(bool is_tx, const char *text)
{
    if (morse.log_file == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    fprintf(morse.log_file, "%s|%u|%s\n", is_tx ? "TX" : "RX", (unsigned)morse.elapsed_ms, text);
    fflush(morse.log_file);
    morse.log_last_flush_ms = morse.elapsed_ms;
    morse.log_loaded = false; /* LOG gorunumu bir sonraki acilista yeniden yuklesin */
}

static void morse_open_log(void)
{
    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_resolve_path("/" MORSE_LOG_DIR, dir, sizeof(dir)) == ESP_OK) {
        (void)solar_os_storage_mkdir(dir);
    }
    if (solar_os_storage_resolve_path("/" MORSE_LOG_DIR "/" MORSE_LOG_FILE, path,
                                      sizeof(path)) != ESP_OK) {
        return;
    }
    morse.log_file = fopen(path, "ab");
}

static void morse_reverse_log_entries(size_t from, size_t to)
{
    while (from < to) {
        const morse_log_entry_t tmp = morse.log_entries[from];
        morse.log_entries[from] = morse.log_entries[to];
        morse.log_entries[to] = tmp;
        from++;
        to--;
    }
}

static void morse_load_log_entries(void)
{
    morse.log_entry_count = 0U;
    morse.log_selected = 0U;
    morse.log_loaded = true;

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_resolve_path("/" MORSE_LOG_DIR "/" MORSE_LOG_FILE, path,
                                      sizeof(path)) != ESP_OK) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    char line[MORSE_LOG_ENTRY_MAX + 24U];
    size_t write_index = 0U;
    while (fgets(line, sizeof(line), f) != NULL) {
        const size_t len = strlen(line);
        if (len > 0U && line[len - 1U] == '\n') {
            line[len - 1U] = '\0';
        }
        const bool is_tx = (strncmp(line, "TX|", 3) == 0);
        const bool is_rx = (strncmp(line, "RX|", 3) == 0);
        if (!is_tx && !is_rx) {
            continue;
        }
        const char *second_bar = strchr(line + 3, '|');
        const char *text = (second_bar != NULL) ? second_bar + 1 : (line + 3);

        morse_log_entry_t *slot = &morse.log_entries[write_index % MORSE_LOG_ENTRIES_MAX];
        slot->is_tx = is_tx;
        strncpy(slot->text, text, MORSE_LOG_ENTRY_MAX - 1U);
        slot->text[MORSE_LOG_ENTRY_MAX - 1U] = '\0';
        write_index++;
    }
    fclose(f);

    morse.log_entry_count = write_index < MORSE_LOG_ENTRIES_MAX ? write_index : MORSE_LOG_ENTRIES_MAX;
    if (write_index > MORSE_LOG_ENTRIES_MAX) {
        /* Dairesel yazim en eski MORSE_LOG_ENTRIES_MAX kaydi (write_index %
         * COUNT) konumundan itibaren tutuyor; kronolojik siraya donmek icin
         * uc-ters-cevirme (three-reversal) rotasyonu uygula. */
        const size_t start = write_index % MORSE_LOG_ENTRIES_MAX;
        if (start > 0U) {
            morse_reverse_log_entries(0U, start - 1U);
            morse_reverse_log_entries(start, MORSE_LOG_ENTRIES_MAX - 1U);
            morse_reverse_log_entries(0U, MORSE_LOG_ENTRIES_MAX - 1U);
        }
    }
    if (morse.log_entry_count > 0U) {
        morse.log_selected = morse.log_entry_count - 1U;
    }
}

/* ---------------------------------------------------------------------
 * Durum satiri
 * ------------------------------------------------------------------- */

static void morse_set_status(const char *message)
{
    strncpy(morse.status_message, message, sizeof(morse.status_message) - 1U);
    morse.status_message[sizeof(morse.status_message) - 1U] = '\0';
    morse.status_until_ms = morse.elapsed_ms + 2500U;
    morse.render_pending = true;
}

/* ---------------------------------------------------------------------
 * Cizim
 * ------------------------------------------------------------------- */

static const char * const MORSE_TAB_NAMES[MORSE_VIEW_COUNT] = { "Send", "Receive", "Log" };

static void morse_build_header(solar_os_appbar_header_t *out, char *status_buf, size_t status_buf_len)
{
    memset(out, 0, sizeof(*out));
    out->title = "Morse";
    out->show_back = true;
    out->tabs.names = MORSE_TAB_NAMES;
    out->tabs.count = MORSE_VIEW_COUNT;
    out->tabs.active_index = (size_t)morse.view;

    const float est_wpm = morse.rx_dit_ms > 0U ? (1200.0f / (float)morse.rx_dit_ms) : 0.0f;
    snprintf(status_buf, status_buf_len, "TX %u WPM  |  RX ~%.0f WPM", (unsigned)morse.wpm, (double)est_wpm);
    out->status_line = status_buf;
}

static void morse_draw_header(solar_os_gfx_t *gfx)
{
    char status_buf[64];
    solar_os_appbar_header_t header;
    morse_build_header(&header, status_buf, sizeof(status_buf));
    solar_os_appbar_draw_header(gfx, &header);
}

/* Builds the current footer's shortcut chips into a caller-owned buffer,
 * returning the count. Same set used by both drawing and click hit-testing
 * so they can never disagree about what's on screen. */
static size_t morse_build_footer_shortcuts(solar_os_appbar_shortcut_t *items, size_t max_items)
{
    size_t n = 0;
    if (morse.view == MORSE_VIEW_SEND) {
        if (n < max_items) { items[n].key = (char)SOLAR_OS_KEY_F4; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Clear"); n++; }
    } else {
        if (n < max_items) { items[n].key = ' '; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "%s", morse.listening ? "Pause" : "Listen"); n++; }
    }
    if (n < max_items) { items[n].key = 'x'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Speed+"); n++; }
    if (n < max_items) { items[n].key = 'z'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Speed-"); n++; }
    return n;
}

static void morse_draw_footer(solar_os_gfx_t *gfx, int width, int height)
{
    if (morse.status_until_ms > morse.elapsed_ms && morse.status_message[0] != '\0') {
        const int footer_h = solar_os_appbar_footer_height(gfx);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_rect(gfx, 0, height - footer_h, width, footer_h);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 8, height - footer_h / 4, morse.status_message);
        return;
    }

    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = morse_build_footer_shortcuts(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);
}

/* Su an gonderilen (ya da az once gonderilmis) harfin mors kodunu buyuk
 * sekillerle cizer: nokta=daire, cizgi=dikdortgen. Aktif sembol, tam o
 * anda ton calarken DOLU, kendi bosluğunda (ton bitmis) ANAHAT olarak
 * gosterilir; tamamlanan semboller anahat, henuz sira gelmemisler soluk
 * anahat. Konum, gorevin tx_current_index'i uzerinden gercek ses
 * ilerlemesiyle senkronize edilir (bkz. morse_sync_send_display). */
static void morse_draw_tx_symbols(solar_os_gfx_t *gfx, int width, int panel_y)
{
    if (morse.send_element_index >= morse.send_element_count) {
        return;
    }
    const morse_element_t *cur = &morse.send_elements[morse.send_element_index];
    const char *code = cur->code;
    if (code == NULL) {
        return;
    }
    const size_t code_len = strlen(code);
    const size_t active_symbol = cur->symbol_index;
    const bool tone_on = morse.send_visual_on;

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    char label[2] = {cur->display_char, '\0'};
    const int label_w = (int)solar_os_gfx_text_width(gfx, label);
    solar_os_gfx_text(gfx, width / 2 - label_w / 2, panel_y + 24, label);

    int dash_w = 70;
    int dot_w = 24;
    int shape_h = 46;
    const int gap = 14;
    const int usable_w = width - 24;
    int worst_w = (int)code_len * (dash_w + gap) - gap;
    if (worst_w > usable_w && code_len > 0U) {
        dash_w = dash_w * usable_w / worst_w;
        dot_w = dot_w * usable_w / worst_w;
        shape_h = shape_h * usable_w / worst_w;
        if (dot_w < 10) dot_w = 10;
        if (dash_w < dot_w * 2) dash_w = dot_w * 2;
        if (shape_h < 16) shape_h = 16;
    }

    int total_w = 0;
    for (size_t i = 0U; i < code_len; i++) {
        total_w += (code[i] == '-') ? dash_w : dot_w;
        if (i + 1U < code_len) total_w += gap;
    }
    int x = width / 2 - total_w / 2;
    const int y = panel_y + 44;

    for (size_t i = 0U; i < code_len; i++) {
        const bool is_dash = (code[i] == '-');
        const int w = is_dash ? dash_w : dot_w;
        const bool active_mark = (i == active_symbol) && tone_on;

        solar_os_gfx_set_color(gfx, (i > active_symbol) ? SOLAR_OS_GFX_COLOR_LIGHT
                                                        : SOLAR_OS_GFX_COLOR_BLACK);
        if (is_dash) {
            if (active_mark) {
                solar_os_gfx_fill_rect(gfx, x, y, w, shape_h);
            } else {
                solar_os_gfx_rect(gfx, x, y, w, shape_h);
            }
        } else {
            const int r = w / 2;
            if (active_mark) {
                solar_os_gfx_fill_circle(gfx, x + r, y + shape_h / 2, r);
            } else {
                solar_os_gfx_circle(gfx, x + r, y + shape_h / 2, r);
            }
        }
        x += w + gap;
    }
}

static void morse_draw_send(solar_os_gfx_t *gfx, int width, int height)
{
    (void)height;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_16);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, 10, MORSE_HEADER_H + 26, "Type:");
    solar_os_gfx_rect(gfx, 8, MORSE_HEADER_H + 34, width - 16, 26);
    solar_os_gfx_text(gfx, 12, MORSE_HEADER_H + 52, morse.compose);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO);
    char morse_preview[MORSE_COMPOSE_MAX * 5U];
    size_t preview_len = 0U;
    for (size_t i = 0U; i < morse.compose_len && preview_len + 8U < sizeof(morse_preview); i++) {
        const char c = morse.compose[i];
        if (c == ' ') {
            morse_preview[preview_len++] = '/';
            morse_preview[preview_len++] = ' ';
            continue;
        }
        const char *code = morse_encode_char(c);
        if (code == NULL) {
            continue;
        }
        for (const char *p = code; *p != '\0'; p++) {
            morse_preview[preview_len++] = *p;
        }
        morse_preview[preview_len++] = ' ';
    }
    morse_preview[preview_len] = '\0';
    solar_os_gfx_text(gfx, 12, MORSE_HEADER_H + 84, morse_preview);

    solar_os_gfx_line(gfx, 8, MORSE_HEADER_H + 96, width - 8, MORSE_HEADER_H + 96);

    if (morse.sending) {
        morse_draw_tx_symbols(gfx, width, MORSE_HEADER_H + 100);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        const char *status_txt = morse.send_visual_on ? "sending..." : "...";
        const int sw = (int)solar_os_gfx_text_width(gfx, status_txt);
        solar_os_gfx_text(gfx, width / 2 - sw / 2, MORSE_HEADER_H + 200, status_txt);
    } else {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        const char *hint = "Enter: send";
        const int hw = (int)solar_os_gfx_text_width(gfx, hint);
        solar_os_gfx_text(gfx, width / 2 - hw / 2, MORSE_HEADER_H + 150, hint);
    }
}

static void morse_draw_level_bar(solar_os_gfx_t *gfx, int x, int y, int w, int h)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, w, h);
    const float threshold = morse.rx_noise_floor * 2.5f + 300.0f;
    float norm = morse.rx_level / (threshold * 2.0f > 1.0f ? threshold * 2.0f : 1.0f);
    if (norm < 0.0f) {
        norm = 0.0f;
    }
    if (norm > 1.0f) {
        norm = 1.0f;
    }
    const int fill_w = (int)((float)(w - 4) * norm);
    if (fill_w > 0) {
        solar_os_gfx_fill_rect(gfx, x + 2, y + 2, fill_w, h - 4);
    }
    const int threshold_x = x + w / 3;
    solar_os_gfx_line(gfx, threshold_x, y, threshold_x, y + h);
}

static void morse_draw_receive(solar_os_gfx_t *gfx, int width, int height)
{
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, 10, MORSE_HEADER_H + 16,
                      morse.listening ? "Listening" : "Paused");
    morse_draw_level_bar(gfx, 10, MORSE_HEADER_H + 22, width - 20, 12);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_16);
    char symbol_line[MORSE_SYMBOL_MAX + 16U];
    snprintf(symbol_line, sizeof(symbol_line), "symbol: %s", morse.rx_symbol_buf);
    solar_os_gfx_text(gfx, 10, MORSE_HEADER_H + 56, symbol_line);

    int y = MORSE_HEADER_H + 84;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO);
    for (size_t i = 0U; i < MORSE_RX_HISTORY_LINES; i++) {
        if (morse.rx_history[i][0] != '\0') {
            solar_os_gfx_text(gfx, 10, y, morse.rx_history[i]);
        }
        y += 20;
    }
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char current_line[MORSE_RX_TEXT_MAX + 4U];
    snprintf(current_line, sizeof(current_line), "%s_", morse.rx_text);
    solar_os_gfx_text(gfx, 10, y, current_line);
    (void)height;
}

static void morse_draw_log(solar_os_gfx_t *gfx, int width, int height)
{
    if (!morse.log_loaded) {
        morse_load_log_entries();
    }
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    if (morse.log_entry_count == 0U) {
        solar_os_gfx_text(gfx, 10, MORSE_HEADER_H + 20, "No logged messages");
        return;
    }
    const int row_h = 18;
    const int top = MORSE_HEADER_H + 6;
    const int max_rows = (height - MORSE_FOOTER_H - top) / row_h;
    size_t start = 0U;
    if (morse.log_entry_count > (size_t)max_rows &&
        morse.log_selected >= (size_t)max_rows) {
        start = morse.log_selected - (size_t)max_rows + 1U;
    }
    for (int row = 0; row < max_rows; row++) {
        const size_t idx = start + (size_t)row;
        if (idx >= morse.log_entry_count) {
            break;
        }
        const int y = top + row * row_h;
        if (idx == morse.log_selected) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
            solar_os_gfx_fill_rect(gfx, 4, y, width - 8, row_h);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }
        char line[MORSE_LOG_ENTRY_MAX + 8U];
        snprintf(line, sizeof(line), "%s  %s", morse.log_entries[idx].is_tx ? ">" : "<",
                  morse.log_entries[idx].text);
        solar_os_gfx_text(gfx, 8, y + row_h - 4, line);
    }
}

static void morse_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    morse_draw_header(gfx);

    switch (morse.view) {
    case MORSE_VIEW_RECEIVE:
        morse_draw_receive(gfx, width, height);
        break;
    case MORSE_VIEW_LOG:
        morse_draw_log(gfx, width, height);
        break;
    case MORSE_VIEW_SEND:
    default:
        morse_draw_send(gfx, width, height);
        break;
    }

    morse_draw_footer(gfx, width, height);
    solar_os_gfx_present(gfx);
    morse.render_pending = false;
}

/* ---------------------------------------------------------------------
 * Olay isleme
 * ------------------------------------------------------------------- */

static void morse_handle_compose_char(char ch)
{
    const unsigned char uch = (unsigned char)ch;
    if (uch == 0x7fU || uch == 0x08U) {
        if (morse.compose_len > 0U) {
            morse.compose_len--;
            morse.compose[morse.compose_len] = '\0';
        }
        morse.render_pending = true;
        return;
    }
    if (uch >= 0x20U && uch < 0x80U && morse.compose_len + 1U < sizeof(morse.compose)) {
        morse.compose[morse.compose_len++] = ch;
        morse.compose[morse.compose_len] = '\0';
        morse.render_pending = true;
    }
}

static void morse_handle_char(solar_os_context_t *ctx, char ch)
{
    const unsigned char uch = (unsigned char)ch;

    if (uch == SOLAR_OS_KEY_ESCAPE) {
        morse_stop_worker();
        morse_stop_tx_worker();
        if (morse.log_file != NULL) {
            fflush(morse.log_file);
            fclose(morse.log_file);
            morse.log_file = NULL;
        }
        solar_os_context_request_exit(ctx);
        return;
    }
    if (ch == '\t') {
        morse.view = (morse_view_t)((morse.view + 1) % MORSE_VIEW_COUNT);
        morse.log_loaded = false;
        morse.render_pending = true;
        return;
    }
    if (ch == ' ' && morse.view != MORSE_VIEW_SEND) {
        if (morse.listening) {
            morse_stop_worker();
            morse_set_status("Listening paused");
        } else {
            (void)morse_start_worker();
            morse_set_status("Listening started");
        }
        return;
    }
    if (uch == SOLAR_OS_KEY_F4) {
        morse.compose_len = 0U;
        morse.compose[0] = '\0';
        morse.render_pending = true;
        return;
    }
    /* Z/X ana kisayollar (bazi klavyelerde/duzenlerde + tusu guvenilir
     * calismiyor); +/- yine de yedek olarak calismaya devam ediyor. */
    if (ch == 'x' || ch == 'X' || ch == '+' || ch == '=') {
        if (morse.wpm < 40U) {
            morse.wpm++;
        }
        morse.render_pending = true;
        return;
    }
    if (ch == 'z' || ch == 'Z' || ch == '-' || ch == '_') {
        if (morse.wpm > 3U) {
            morse.wpm--;
        }
        morse.render_pending = true;
        return;
    }

    if (morse.view == MORSE_VIEW_SEND) {
        if (ch == '\n' || ch == '\r') {
            morse_start_send();
            return;
        }
        morse_handle_compose_char(ch);
        return;
    }

    if (morse.view == MORSE_VIEW_LOG) {
        if (uch == SOLAR_OS_KEY_UP) {
            if (morse.log_selected > 0U) {
                morse.log_selected--;
            }
            morse.render_pending = true;
            return;
        }
        if (uch == SOLAR_OS_KEY_DOWN) {
            if (morse.log_selected + 1U < morse.log_entry_count) {
                morse.log_selected++;
            }
            morse.render_pending = true;
            return;
        }
        if (ch == '\n' || ch == '\r') {
            if (morse.log_selected < morse.log_entry_count &&
                morse.log_entries[morse.log_selected].is_tx) {
                strncpy(morse.compose, morse.log_entries[morse.log_selected].text,
                        sizeof(morse.compose) - 1U);
                morse.compose[sizeof(morse.compose) - 1U] = '\0';
                morse.compose_len = strlen(morse.compose);
                morse.view = MORSE_VIEW_SEND;
                morse_start_send();
            }
            return;
        }
    }
}

static bool morse_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    switch (event->type) {
    case SOLAR_OS_EVENT_CHAR:
        morse_handle_char(ctx, event->data.ch);
        break;
    case SOLAR_OS_EVENT_TICK:
        morse.elapsed_ms += MORSE_TICK_MS;
        if (morse.listening) {
            morse_decode_tick();
        }
        if (morse.sending) {
            morse_sync_send_display();
        }
        morse_reap_tx_worker();
        if (morse.status_until_ms != 0U && morse.status_until_ms <= morse.elapsed_ms &&
            morse.status_until_ms + MORSE_TICK_MS > morse.elapsed_ms) {
            morse.render_pending = true;
        }
        if (morse.render_pending) {
            morse_render(ctx);
        }
        break;
    case SOLAR_OS_EVENT_RESUME:
        morse.render_pending = true;
        morse_render(ctx);
        break;

    case SOLAR_OS_EVENT_SCROLL:
        if (morse.view == MORSE_VIEW_LOG) {
            const bool down = event->data.scroll.delta < 0;
            if (down) {
                if (morse.log_selected + 1U < morse.log_entry_count) {
                    morse.log_selected++;
                    morse.render_pending = true;
                }
            } else if (morse.log_selected > 0U) {
                morse.log_selected--;
                morse.render_pending = true;
            }
            if (morse.render_pending) morse_render(ctx);
        }
        break;

    case SOLAR_OS_EVENT_CLICK: {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) break;

        char status_buf[64];
        solar_os_appbar_header_t header;
        morse_build_header(&header, status_buf, sizeof(status_buf));

        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, event->data.click.x, event->data.click.y, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                morse_handle_char(ctx, (char)SOLAR_OS_KEY_ESCAPE);
            } else if (hit.kind == SOLAR_OS_APPBAR_HIT_TAB_ITEM && hit.index < MORSE_VIEW_COUNT) {
                morse.view = (morse_view_t)hit.index;
                morse.log_loaded = false;
                morse.render_pending = true;
            }
            if (morse.render_pending) morse_render(ctx);
            break;
        }

        const bool showing_status = morse.status_until_ms > morse.elapsed_ms && morse.status_message[0] != '\0';
        if (!showing_status) {
            solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
            const size_t count = morse_build_footer_shortcuts(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
            const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };

            solar_os_appbar_hit_t fhit;
            if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, event->data.click.x, event->data.click.y, &fhit) &&
                fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM) {
                morse_handle_char(ctx, items[fhit.index].key);
                if (morse.render_pending) morse_render(ctx);
            }
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

static esp_err_t morse_start(solar_os_context_t *ctx)
{
    memset(&morse, 0, sizeof(morse));
    morse.wpm = 15U;
    morse.view = MORSE_VIEW_SEND;
    morse.render_pending = true;

    morse_open_log();
    solar_os_context_set_graphics_active(ctx, true);

    const esp_err_t worker_err = morse_start_worker();
    if (worker_err != ESP_OK) {
        morse_set_status("Could not start microphone");
    }

    morse_render(ctx);
    return ESP_OK;
}

static void morse_suspend(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
    morse_stop_worker();
    morse_stop_tx_worker();
}

static void morse_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    if (!morse.listening) {
        (void)morse_start_worker();
    }
    morse.render_pending = true;
    morse_render(ctx);
}

static void morse_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    morse_stop_worker();
    morse_stop_tx_worker();
    if (morse.log_file != NULL) {
        fflush(morse.log_file);
        fclose(morse.log_file);
        morse.log_file = NULL;
    }
}

static void morse_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    snprintf(buffer, buffer_len, "Morse: %s", morse.listening ? "listening" : "paused");
}

const solar_os_app_t solar_os_mors_app = {
    .name = "mors",
    .summary = "automatic Morse code receiver/transmitter",
    .flags = 0,
    .start = morse_start,
    .suspend = morse_suspend,
    .resume = morse_resume,
    .stop = morse_stop,
    .event = morse_event,
    .title = morse_title,
    .state_slot = &morse_state_ptr,
    .state_size = sizeof(morse_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = MORSE_TASK_STACK,
    .worker_stack_external = false,
    .tick_interval_ms = MORSE_TICK_MS,
    .tick_deadline_ms = MORSE_TICK_MS * 3U,
    .requested_tick_interval_ms = NULL,
};
