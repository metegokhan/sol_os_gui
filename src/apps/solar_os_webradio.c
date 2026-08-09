#include "solar_os_webradio.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "solar_os_audio.h"
#include "solar_os_audio_codec.h"
#include "solar_os_audio_pcm.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_gfx.h"
#include "solar_os_http_client.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_shell_io.h"
#include "solar_os_stream.h"
#include "solar_os_task.h"
#include "solar_os_tui.h"
#include "solar_os_webradio_catalog.h"

#define WEBRADIO_TASK_STACK 20480U
#define WEBRADIO_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define WEBRADIO_PLAYBACK_TASK_STACK 8192U
#define WEBRADIO_PLAYBACK_TASK_PRIORITY (WEBRADIO_TASK_PRIORITY + 1U)
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(WEBRADIO_PLAYBACK_TASK_STACK);
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(WEBRADIO_TASK_STACK);
#endif
#define WEBRADIO_HTTP_TIMEOUT_MS 10000U
#define WEBRADIO_RECONNECT_DELAY_MS 2000U
#define WEBRADIO_INPUT_BYTES 16384U
#define WEBRADIO_PCM_BUFFER_BYTES 4096U
#define WEBRADIO_OUTPUT_SAMPLES (WEBRADIO_PCM_BUFFER_BYTES / sizeof(int16_t))
#define WEBRADIO_JITTER_EXTERNAL_BYTES (128U * 1024U)
#define WEBRADIO_JITTER_INTERNAL_BYTES (32U * 1024U)
#define WEBRADIO_JITTER_TARGET_MS 500U
#define WEBRADIO_WORKER_POLL_MS 20U
#define WEBRADIO_INVALID_STREAM_BYTES (64U * 1024U)
#define WEBRADIO_GUI_HEADER_HEIGHT 24
#define WEBRADIO_GUI_STATUS_HEIGHT 42
#define WEBRADIO_GUI_ROW_HEIGHT 24
#define WEBRADIO_GUI_FOOTER_HEIGHT 18

typedef enum {
    WEBRADIO_MODE_TUI = 0,
    WEBRADIO_MODE_GRAPHICS,
} webradio_mode_t;

typedef enum {
    WEBRADIO_PLAYBACK_IDLE = 0,
    WEBRADIO_PLAYBACK_CONNECTING,
    WEBRADIO_PLAYBACK_BUFFERING,
    WEBRADIO_PLAYBACK_PLAYING,
    WEBRADIO_PLAYBACK_RECONNECTING,
    WEBRADIO_PLAYBACK_ERROR,
} webradio_playback_state_t;

typedef struct {
    solar_os_stream_handle_t stream;
    StreamBufferHandle_t jitter;
    size_t jitter_capacity;
    bool jitter_external;
    solar_os_audio_mp3_decoder_t *decoder;
    solar_os_audio_s16_converter_t converter;
    uint8_t *input;
    size_t input_len;
    int16_t *decoded;
    int16_t *output;
    int16_t *playback;
    int16_t *sink;
    size_t playback_samples;
    uint64_t network_bytes;
    uint64_t output_bytes;
    bool decoded_any;
    volatile bool playback_ready;
    volatile bool playback_done;
    volatile bool playback_stop;
    volatile bool playback_started;
    volatile esp_err_t playback_error;
    TaskHandle_t playback_task;
    char content_type[64];
} webradio_worker_t;

typedef struct {
    webradio_mode_t mode;
    solar_os_tui_t tui;
    solar_os_shell_io_t fallback_io;
    solar_os_webradio_station_t stations[SOLAR_OS_WEBRADIO_STATION_MAX];
    size_t station_count;
    size_t cursor;
    size_t top;
    uint32_t catalog_generation;
    bool ui_started;
    bool suspended;
    bool redraw;
    volatile bool stop_requested;
    volatile bool task_done;
    TaskHandle_t task;
    solar_os_http_request_t *request;
    webradio_playback_state_t playback_state;
    char playback_message[96];
    char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    char active_url[SOLAR_OS_WEBRADIO_URL_MAX];
    uint32_t source_rate;
    uint8_t source_channels;
} webradio_app_state_t;

static const char *TAG = "solar_os_webradio";
static EXT_RAM_BSS_ATTR webradio_app_state_t webradio;
static portMUX_TYPE webradio_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *webradio_state_name(webradio_playback_state_t state)
{
    switch (state) {
    case WEBRADIO_PLAYBACK_CONNECTING:
        return "connecting";
    case WEBRADIO_PLAYBACK_BUFFERING:
        return "buffering";
    case WEBRADIO_PLAYBACK_PLAYING:
        return "playing";
    case WEBRADIO_PLAYBACK_RECONNECTING:
        return "reconnecting";
    case WEBRADIO_PLAYBACK_ERROR:
        return "error";
    case WEBRADIO_PLAYBACK_IDLE:
    default:
        return "stopped";
    }
}

static void webradio_set_playback_state(webradio_playback_state_t state,
                                        const char *message)
{
    portENTER_CRITICAL(&webradio_lock);
    webradio.playback_state = state;
    if (message != NULL) {
        strlcpy(webradio.playback_message,
                message,
                sizeof(webradio.playback_message));
    } else {
        webradio.playback_message[0] = '\0';
    }
    webradio.redraw = true;
    portEXIT_CRITICAL(&webradio_lock);
}

static void webradio_publish_source(
    const solar_os_stream_audio_format_t *source)
{
    portENTER_CRITICAL(&webradio_lock);
    if (source != NULL) {
        const bool format_changed =
            webradio.source_rate != source->sample_rate ||
            webradio.source_channels != source->channels;
        webradio.source_rate = source->sample_rate;
        webradio.source_channels = source->channels;
        if (format_changed) {
            webradio.redraw = true;
        }
    }
    portEXIT_CRITICAL(&webradio_lock);
}

static solar_os_shell_io_t *webradio_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&webradio.fallback_io,
                                        solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &webradio.fallback_io);
        io = &webradio.fallback_io;
    }
    return io;
}

static bool webradio_graphical_session(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = webradio_io(ctx);
    return solar_os_context_gfx(ctx) != NULL &&
        (io == NULL || solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT);
}

static void webradio_clip(char *destination,
                          size_t destination_len,
                          const char *source,
                          size_t width)
{
    if (destination == NULL || destination_len == 0U) {
        return;
    }
    if (source == NULL) {
        source = "";
    }
    const size_t limit = width < destination_len - 1U ? width : destination_len - 1U;
    const size_t length = strnlen(source, limit);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void webradio_refresh_catalog(void)
{
    webradio.station_count = solar_os_webradio_catalog_snapshot(
        webradio.stations,
        SOLAR_OS_WEBRADIO_STATION_MAX,
        &webradio.catalog_generation);
    if (webradio.station_count == 0U) {
        webradio.cursor = 0U;
        webradio.top = 0U;
    } else if (webradio.cursor >= webradio.station_count) {
        webradio.cursor = webradio.station_count - 1U;
    }
}

static void webradio_snapshot_status(webradio_playback_state_t *state,
                                     char *message,
                                     size_t message_len,
                                     char *active_name,
                                     size_t active_name_len,
                                     uint32_t *source_rate,
                                     uint8_t *source_channels,
                                     bool *redraw)
{
    portENTER_CRITICAL(&webradio_lock);
    if (state != NULL) {
        *state = webradio.playback_state;
    }
    if (message != NULL && message_len > 0U) {
        strlcpy(message, webradio.playback_message, message_len);
    }
    if (active_name != NULL && active_name_len > 0U) {
        strlcpy(active_name, webradio.active_name, active_name_len);
    }
    if (source_rate != NULL) {
        *source_rate = webradio.source_rate;
    }
    if (source_channels != NULL) {
        *source_channels = webradio.source_channels;
    }
    if (redraw != NULL) {
        *redraw = webradio.redraw;
        webradio.redraw = false;
    }
    portEXIT_CRITICAL(&webradio_lock);
}

static void webradio_render_tui(void)
{
    const size_t rows = solar_os_tui_rows(&webradio.tui);
    const size_t cols = solar_os_tui_cols(&webradio.tui);
    if (rows < 5U || cols < 20U) {
        return;
    }

    webradio_playback_state_t state;
    char message[96];
    char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    uint32_t sample_rate = 0U;
    uint8_t channels = 0U;
    webradio_snapshot_status(&state,
                             message,
                             sizeof(message),
                             active_name,
                             sizeof(active_name),
                             &sample_rate,
                             &channels,
                             NULL);

    solar_os_tui_clear(&webradio.tui);
    solar_os_tui_fill(&webradio.tui,
                      0U,
                      0U,
                      1U,
                      cols,
                      ' ',
                      SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    solar_os_tui_addstr(&webradio.tui,
                        0U,
                        1U,
                        "WebRadio",
                        SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);

    char status[192];
    if (state == WEBRADIO_PLAYBACK_PLAYING) {
        snprintf(status,
                 sizeof(status),
                 "%s: %s | %" PRIu32 " Hz, %u ch",
                 webradio_state_name(state),
                 active_name,
                 sample_rate,
                 (unsigned)channels);
    } else {
        snprintf(status,
                 sizeof(status),
                 "%s%s%s",
                 webradio_state_name(state),
                 message[0] != '\0' ? ": " : "",
                 message);
    }
    char clipped[192];
    webradio_clip(clipped, sizeof(clipped), status, cols > 2U ? cols - 2U : 0U);
    solar_os_tui_addstr(&webradio.tui,
                        1U,
                        1U,
                        clipped,
                        state == WEBRADIO_PLAYBACK_ERROR ?
                            SOLAR_OS_TUI_ATTR_BOLD : SOLAR_OS_TUI_ATTR_NORMAL);

    const size_t list_rows = rows - 4U;
    if (webradio.cursor < webradio.top) {
        webradio.top = webradio.cursor;
    }
    if (webradio.cursor >= webradio.top + list_rows) {
        webradio.top = webradio.cursor - list_rows + 1U;
    }
    if (webradio.station_count == 0U) {
        solar_os_tui_addstr(&webradio.tui,
                            3U,
                            2U,
                            "Catalog is empty. Use: webradio add NAME URL",
                            SOLAR_OS_TUI_ATTR_NORMAL);
    } else {
        for (size_t row = 0U; row < list_rows; row++) {
            const size_t index = webradio.top + row;
            if (index >= webradio.station_count) {
                break;
            }
            char line[192];
            const bool active = active_name[0] != '\0' &&
                strcmp(active_name, webradio.stations[index].name) == 0 &&
                state != WEBRADIO_PLAYBACK_IDLE &&
                state != WEBRADIO_PLAYBACK_ERROR;
            snprintf(line,
                     sizeof(line),
                     "%c %-20s %s",
                     active ? '*' : ' ',
                     webradio.stations[index].name,
                     webradio.stations[index].url);
            webradio_clip(clipped,
                          sizeof(clipped),
                          line,
                          cols > 2U ? cols - 2U : 0U);
            solar_os_tui_addstr(&webradio.tui,
                                row + 3U,
                                1U,
                                clipped,
                                index == webradio.cursor ?
                                    SOLAR_OS_TUI_ATTR_INVERSE :
                                    SOLAR_OS_TUI_ATTR_NORMAL);
        }
    }
    solar_os_tui_addstr(&webradio.tui,
                        rows - 1U,
                        1U,
                        "Enter play  Space stop  Del remove  R retry  Q exit",
                        SOLAR_OS_TUI_ATTR_BOLD);
    solar_os_tui_set_cursor_visible(&webradio.tui, false);
    solar_os_tui_refresh(&webradio.tui);
}

static void webradio_render_graphics(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    webradio_playback_state_t state;
    char message[96];
    char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    uint32_t sample_rate = 0U;
    uint8_t channels = 0U;
    webradio_snapshot_status(&state,
                             message,
                             sizeof(message),
                             active_name,
                             sizeof(active_name),
                             &sample_rate,
                             &channels,
                             NULL);

    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, WEBRADIO_GUI_HEADER_HEIGHT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_text(gfx, 8, 18, "WebRadio");

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx,
                      6,
                      WEBRADIO_GUI_HEADER_HEIGHT + 6,
                      width - 12,
                      WEBRADIO_GUI_STATUS_HEIGHT);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_14);
    solar_os_gfx_text(gfx,
                      14,
                      WEBRADIO_GUI_HEADER_HEIGHT + 23,
                      active_name[0] != '\0' ? active_name : "No station selected");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    char status[96];
    if (state == WEBRADIO_PLAYBACK_PLAYING) {
        snprintf(status,
                 sizeof(status),
                 "playing  %" PRIu32 " Hz  %u ch",
                 sample_rate,
                 (unsigned)channels);
    } else {
        snprintf(status,
                 sizeof(status),
                 "%s%s%s",
                 webradio_state_name(state),
                 message[0] != '\0' ? " - " : "",
                 message);
    }
    char clipped[96];
    webradio_clip(clipped, sizeof(clipped), status, (size_t)(width / 7));
    solar_os_gfx_text(gfx, 14, WEBRADIO_GUI_HEADER_HEIGHT + 42, clipped);

    const int list_y = WEBRADIO_GUI_HEADER_HEIGHT + WEBRADIO_GUI_STATUS_HEIGHT + 14;
    const int list_bottom = height - WEBRADIO_GUI_FOOTER_HEIGHT;
    const size_t visible_rows = list_bottom > list_y ?
        (size_t)((list_bottom - list_y) / WEBRADIO_GUI_ROW_HEIGHT) : 0U;
    if (webradio.cursor < webradio.top) {
        webradio.top = webradio.cursor;
    }
    if (visible_rows > 0U && webradio.cursor >= webradio.top + visible_rows) {
        webradio.top = webradio.cursor - visible_rows + 1U;
    }

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_14);
    for (size_t row = 0U; row < visible_rows; row++) {
        const size_t index = webradio.top + row;
        if (index >= webradio.station_count) {
            break;
        }
        const int y = list_y + (int)row * WEBRADIO_GUI_ROW_HEIGHT;
        if (index == webradio.cursor) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 6, y, width - 12, WEBRADIO_GUI_ROW_HEIGHT - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }
        const bool active = active_name[0] != '\0' &&
            strcmp(active_name, webradio.stations[index].name) == 0 &&
            state != WEBRADIO_PLAYBACK_IDLE &&
            state != WEBRADIO_PLAYBACK_ERROR;
        solar_os_gfx_text(gfx, 14, y + 17, active ? ">" : " ");
        solar_os_gfx_text(gfx, 30, y + 17, webradio.stations[index].name);
    }
    if (webradio.station_count == 0U) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_text(gfx, 14, list_y + 17, "Catalog is empty");
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx,
                      8,
                      height - 5,
                      "UP/DOWN select   ENTER play   SPACE stop   DEL remove");
    solar_os_gfx_present(gfx);
}

static void webradio_render(solar_os_context_t *ctx)
{
    if (webradio.suspended) {
        return;
    }
    if (webradio.mode == WEBRADIO_MODE_GRAPHICS) {
        webradio_render_graphics(ctx);
    } else {
        webradio_render_tui();
    }
}

static void webradio_consume_input(webradio_worker_t *worker, size_t consumed)
{
    if (consumed >= worker->input_len) {
        worker->input_len = 0U;
        return;
    }
    memmove(worker->input,
            worker->input + consumed,
            worker->input_len - consumed);
    worker->input_len -= consumed;
}

static esp_err_t webradio_playback_flush(webradio_worker_t *worker, bool pad_tail)
{
    if (worker->playback_samples == 0U) {
        return ESP_OK;
    }
    if (pad_tail) {
        const size_t frames_per_block = worker->stream.audio.frames_per_block != 0U ?
            worker->stream.audio.frames_per_block : 256U;
        const size_t quantum = frames_per_block * worker->stream.audio.channels;
        size_t padded =
            ((worker->playback_samples + quantum - 1U) / quantum) * quantum;
        if (padded > WEBRADIO_OUTPUT_SAMPLES) {
            padded = WEBRADIO_OUTPUT_SAMPLES;
        }
        while (worker->playback_samples < padded) {
            worker->playback[worker->playback_samples++] = 0;
        }
    }
    const size_t bytes = worker->playback_samples * sizeof(worker->playback[0]);
    size_t sent = 0U;
    while (sent < bytes && !webradio.stop_requested &&
           !worker->playback_stop && worker->playback_error == ESP_OK) {
        const size_t count = xStreamBufferSend(
            worker->jitter,
            (const uint8_t *)worker->playback + sent,
            bytes - sent,
            pdMS_TO_TICKS(WEBRADIO_WORKER_POLL_MS));
        sent += count;
    }
    if (sent != bytes) {
        return worker->playback_error != ESP_OK ?
            worker->playback_error : ESP_ERR_INVALID_STATE;
    }
    worker->playback_samples = 0U;
    return ESP_OK;
}

static esp_err_t webradio_playback_append(webradio_worker_t *worker,
                                          const int16_t *samples,
                                          size_t sample_count)
{
    while (sample_count > 0U) {
        if (worker->playback_samples == WEBRADIO_OUTPUT_SAMPLES) {
            const esp_err_t err = webradio_playback_flush(worker, false);
            if (err != ESP_OK) {
                return err;
            }
        }
        const size_t space = WEBRADIO_OUTPUT_SAMPLES - worker->playback_samples;
        const size_t count = sample_count < space ? sample_count : space;
        memcpy(worker->playback + worker->playback_samples,
               samples,
               count * sizeof(samples[0]));
        worker->playback_samples += count;
        samples += count;
        sample_count -= count;
    }
    return ESP_OK;
}

static esp_err_t webradio_decode_available(webradio_worker_t *worker)
{
    while (worker->input_len >= SOLAR_OS_AUDIO_MP3_STREAM_WINDOW_BYTES &&
           !webradio.stop_requested) {
        solar_os_audio_decoded_frame_t frame;
        size_t consumed = 0U;
        esp_err_t err = solar_os_audio_mp3_decode(
            worker->decoder,
            worker->input,
            worker->input_len,
            &consumed,
            worker->decoded,
            SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES,
            &frame);
        if (err != ESP_OK) {
            return err;
        }

        if (frame.frames > 0U) {
            const bool first_frame = !worker->decoded_any;
            if (first_frame) {
                worker->decoded_any = true;
                if (worker->playback_started) {
                    webradio_set_playback_state(WEBRADIO_PLAYBACK_PLAYING, NULL);
                }
            }
            bool source_done = false;
            do {
                size_t output_samples = 0U;
                err = solar_os_audio_s16_convert(
                    &worker->converter,
                    worker->decoded,
                    frame.frames,
                    &frame.format,
                    &worker->stream.audio,
                    worker->output,
                    WEBRADIO_OUTPUT_SAMPLES,
                    &output_samples,
                    &source_done);
                if (err != ESP_OK) {
                    return err;
                }
                err = webradio_playback_append(worker,
                                               worker->output,
                                               output_samples);
                if (err != ESP_OK) {
                    return err;
                }
                worker->output_bytes += output_samples * sizeof(worker->output[0]);
            } while (!source_done && !webradio.stop_requested);
            if (first_frame) {
                webradio_publish_source(&frame.format);
            }
        }

        if (consumed == 0U) {
            break;
        }
        webradio_consume_input(worker, consumed);
    }
    return webradio.stop_requested ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t webradio_feed_mp3(webradio_worker_t *worker,
                                   const uint8_t *data,
                                   size_t length)
{
    while (length > 0U && !webradio.stop_requested) {
        if (worker->input_len == WEBRADIO_INPUT_BYTES) {
            esp_err_t err = webradio_decode_available(worker);
            if (err != ESP_OK) {
                return err;
            }
            if (worker->input_len == WEBRADIO_INPUT_BYTES) {
                webradio_consume_input(worker, 1U);
            }
        }
        const size_t space = WEBRADIO_INPUT_BYTES - worker->input_len;
        const size_t count = length < space ? length : space;
        memcpy(worker->input + worker->input_len, data, count);
        worker->input_len += count;
        worker->network_bytes += count;
        data += count;
        length -= count;

        const esp_err_t err = webradio_decode_available(worker);
        if (err != ESP_OK) {
            return err;
        }
        if (!worker->decoded_any && worker->network_bytes >= WEBRADIO_INVALID_STREAM_BYTES) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    return webradio.stop_requested ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t webradio_http_event(const solar_os_http_event_t *event,
                                     void *user_data)
{
    webradio_worker_t *worker = user_data;
    if (event == NULL || worker == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (webradio.stop_requested) {
        return ESP_ERR_INVALID_STATE;
    }
    if (event->type == SOLAR_OS_HTTP_EVENT_HEADER) {
        if (event->header_name != NULL && event->header_value != NULL &&
            strcasecmp(event->header_name, "Content-Type") == 0) {
            strlcpy(worker->content_type,
                    event->header_value,
                    sizeof(worker->content_type));
        }
        return ESP_OK;
    }
    if (event->type != SOLAR_OS_HTTP_EVENT_DATA) {
        return ESP_OK;
    }
    if (event->status_code < 200 || event->status_code >= 300) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strncasecmp(worker->content_type, "text/", 5U) == 0 ||
        strncasecmp(worker->content_type, "application/json", 16U) == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return webradio_feed_mp3(worker, event->data, event->data_len);
}

static StreamBufferHandle_t webradio_jitter_create(webradio_worker_t *worker)
{
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
    StreamBufferHandle_t jitter = xStreamBufferCreateWithCaps(
        WEBRADIO_JITTER_EXTERNAL_BYTES,
        WEBRADIO_PCM_BUFFER_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jitter != NULL) {
        worker->jitter_capacity = WEBRADIO_JITTER_EXTERNAL_BYTES;
        worker->jitter_external = true;
        return jitter;
    }
#endif
    worker->jitter_capacity = WEBRADIO_JITTER_INTERNAL_BYTES;
    worker->jitter_external = false;
    return xStreamBufferCreate(WEBRADIO_JITTER_INTERNAL_BYTES,
                               WEBRADIO_PCM_BUFFER_BYTES);
}

static void webradio_jitter_delete(webradio_worker_t *worker)
{
    if (worker->jitter == NULL) {
        return;
    }
    if (worker->jitter_external) {
        vStreamBufferDeleteWithCaps(worker->jitter);
    } else {
        vStreamBufferDelete(worker->jitter);
    }
    worker->jitter = NULL;
}

static size_t webradio_jitter_target(const webradio_worker_t *worker)
{
    const uint64_t bytes_per_second =
        (uint64_t)worker->stream.audio.sample_rate *
        worker->stream.audio.channels * sizeof(int16_t);
    size_t target = (size_t)((bytes_per_second * WEBRADIO_JITTER_TARGET_MS) /
                             1000U);
    const size_t maximum = (worker->jitter_capacity * 3U) / 4U;
    if (target > maximum) {
        target = maximum;
    }
    if (target < WEBRADIO_PCM_BUFFER_BYTES) {
        target = WEBRADIO_PCM_BUFFER_BYTES;
    }
    return target;
}

static void webradio_playback_task(void *arg)
{
    webradio_worker_t *worker = arg;
    worker->stream = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    const solar_os_stream_open_options_t options = {
        .direction = SOLAR_OS_STREAM_DIRECTION_SINK,
        .timeout_ms = UINT32_MAX,
        .requested_audio = {
            .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
            .bits_per_sample = 16U,
        },
    };
    esp_err_t err = solar_os_audio_open_default(
        SOLAR_OS_STREAM_DIRECTION_SINK,
        "webradio",
        &options,
        &worker->stream,
        NULL);
    const bool stream_opened = err == ESP_OK;
    if (err == ESP_OK &&
        (worker->stream.audio.sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
         worker->stream.audio.bits_per_sample != 16U ||
         worker->stream.audio.sample_rate == 0U ||
         worker->stream.audio.channels == 0U ||
         worker->stream.audio.channels > 2U)) {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    const bool stream_supported = err == ESP_OK;
    worker->playback_error = err;
    worker->playback_ready = true;

    if (stream_supported) {
        const size_t target = webradio_jitter_target(worker);
        size_t filled = 0U;
        bool primed = false;
        while (!webradio.stop_requested && !worker->playback_stop) {
            if (!primed) {
                const size_t available =
                    xStreamBufferBytesAvailable(worker->jitter);
                if (filled + available < target) {
                    vTaskDelay(pdMS_TO_TICKS(WEBRADIO_WORKER_POLL_MS));
                    continue;
                }
                primed = true;
                worker->playback_started = true;
                webradio_set_playback_state(WEBRADIO_PLAYBACK_PLAYING, NULL);
            }

            const size_t received = xStreamBufferReceive(
                worker->jitter,
                (uint8_t *)worker->sink + filled,
                WEBRADIO_PCM_BUFFER_BYTES - filled,
                pdMS_TO_TICKS(WEBRADIO_WORKER_POLL_MS));
            if (received == 0U) {
                primed = false;
                worker->playback_started = false;
                webradio_set_playback_state(WEBRADIO_PLAYBACK_BUFFERING, NULL);
                continue;
            }
            filled += received;
            if (filled < WEBRADIO_PCM_BUFFER_BYTES) {
                continue;
            }

            size_t written = 0U;
            err = solar_os_stream_write(&worker->stream,
                                        worker->sink,
                                        WEBRADIO_PCM_BUFFER_BYTES,
                                        0U,
                                        &written);
            if (err != ESP_OK || written != WEBRADIO_PCM_BUFFER_BYTES) {
                worker->playback_error = err != ESP_OK ?
                    err : ESP_ERR_INVALID_SIZE;
                webradio_set_playback_state(WEBRADIO_PLAYBACK_ERROR,
                                            "audio output failed");
                break;
            }
            filled = 0U;
        }

        memset(worker->sink, 0, WEBRADIO_PCM_BUFFER_BYTES);
        size_t written = 0U;
        (void)solar_os_stream_write(&worker->stream,
                                    worker->sink,
                                    WEBRADIO_PCM_BUFFER_BYTES,
                                    0U,
                                    &written);
    }
    if (stream_opened) {
        solar_os_stream_close(&worker->stream);
    }

    worker->playback_started = false;
    worker->playback_done = true;
    solar_os_task_delete_internal(NULL);
}

static void webradio_worker_free(webradio_worker_t *worker)
{
    worker->playback_stop = true;
    if (worker->playback_task != NULL &&
        !solar_os_task_wait_done(worker->playback_task,
                                 &worker->playback_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "playback worker is slow to stop");
        while (!worker->playback_done) {
            vTaskDelay(pdMS_TO_TICKS(WEBRADIO_WORKER_POLL_MS));
        }
    }
    worker->playback_task = NULL;
    solar_os_audio_mp3_decoder_destroy(worker->decoder);
    solar_os_memory_free(worker->input);
    solar_os_memory_free(worker->decoded);
    solar_os_memory_free(worker->output);
    solar_os_memory_free(worker->playback);
    solar_os_memory_free(worker->sink);
    webradio_jitter_delete(worker);
    memset(worker, 0, sizeof(*worker));
}

static esp_err_t webradio_worker_init(webradio_worker_t *worker)
{
    memset(worker, 0, sizeof(*worker));
    worker->playback_error = ESP_OK;
    worker->input = solar_os_memory_alloc(WEBRADIO_INPUT_BYTES,
                                           SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                           "webradio.input");
    worker->decoded = solar_os_memory_alloc(
        SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES * sizeof(worker->decoded[0]),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "webradio.decoded");
    worker->output = solar_os_memory_alloc(WEBRADIO_PCM_BUFFER_BYTES,
                                            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                            "webradio.output");
    worker->playback = solar_os_memory_alloc(WEBRADIO_PCM_BUFFER_BYTES,
                                              SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                              "webradio.playback");
    worker->sink = solar_os_memory_alloc(WEBRADIO_PCM_BUFFER_BYTES,
                                          SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                          "webradio.sink");
    worker->jitter = webradio_jitter_create(worker);
    if (worker->input == NULL || worker->decoded == NULL ||
        worker->output == NULL || worker->playback == NULL ||
        worker->sink == NULL || worker->jitter == NULL) {
        webradio_worker_free(worker);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = solar_os_task_create_pinned_internal(
        webradio_playback_task,
        "webradio_audio",
        WEBRADIO_PLAYBACK_TASK_STACK,
        worker,
        WEBRADIO_PLAYBACK_TASK_PRIORITY,
        &worker->playback_task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        webradio_worker_free(worker);
        return ESP_ERR_NO_MEM;
    }
    while (!worker->playback_ready && !worker->playback_done &&
           !webradio.stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(WEBRADIO_WORKER_POLL_MS));
    }
    if (worker->playback_error != ESP_OK) {
        const esp_err_t err = worker->playback_error;
        webradio_worker_free(worker);
        return err;
    }
    if (webradio.stop_requested) {
        webradio_worker_free(worker);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void webradio_request_publish(solar_os_http_request_t *request)
{
    portENTER_CRITICAL(&webradio_lock);
    webradio.request = request;
    portEXIT_CRITICAL(&webradio_lock);
}

static void webradio_network_task(void *arg)
{
    (void)arg;
    webradio_worker_t worker;
    esp_err_t err = webradio_worker_init(&worker);
    if (err != ESP_OK) {
        webradio_set_playback_state(
            WEBRADIO_PLAYBACK_ERROR,
            err == ESP_ERR_NOT_FOUND ? "no audio output device" :
            err == ESP_ERR_NO_MEM ? "not enough memory" :
            "audio output unavailable");
        goto done;
    }

    const solar_os_http_header_t headers[] = {
        {"Accept", "audio/mpeg, audio/mp3, application/octet-stream"},
    };
    while (!webradio.stop_requested) {
        worker.input_len = 0U;
        worker.playback_samples = 0U;
        worker.decoded_any = false;
        worker.content_type[0] = '\0';
        solar_os_audio_s16_converter_reset(&worker.converter);
        solar_os_audio_mp3_decoder_destroy(worker.decoder);
        worker.decoder = NULL;
        err = solar_os_audio_mp3_decoder_create(&worker.decoder);
        if (err != ESP_OK) {
            webradio_set_playback_state(WEBRADIO_PLAYBACK_ERROR,
                                        "decoder allocation failed");
            break;
        }

        webradio_set_playback_state(WEBRADIO_PLAYBACK_CONNECTING, NULL);
        const solar_os_http_request_options_t options = {
            .url = webradio.active_url,
            .method = SOLAR_OS_HTTP_METHOD_GET,
            .headers = headers,
            .header_count = sizeof(headers) / sizeof(headers[0]),
            .user_agent = "SolarOS-WebRadio/0.1",
            .follow_redirects = true,
            .max_redirects = 5U,
            .timeout_ms = WEBRADIO_HTTP_TIMEOUT_MS,
            .receive_buffer_size = 2048U,
            .transmit_buffer_size = 512U,
            .event_handler = webradio_http_event,
            .user_data = &worker,
        };
        solar_os_http_request_t *request = NULL;
        err = solar_os_http_request_create(&options, &request);
        if (err != ESP_OK) {
            webradio_set_playback_state(WEBRADIO_PLAYBACK_ERROR,
                                        "HTTP client allocation failed");
            break;
        }
        webradio_request_publish(request);
        webradio_set_playback_state(WEBRADIO_PLAYBACK_BUFFERING, NULL);
        solar_os_http_response_t response;
        err = solar_os_http_request_perform(request, &response);
        webradio_request_publish(NULL);
        (void)solar_os_http_request_destroy(request);

        (void)webradio_playback_flush(&worker, true);
        if (webradio.stop_requested) {
            break;
        }
        if (worker.playback_error != ESP_OK) {
            break;
        }
        const bool http_rejected = response.status_code >= 0 &&
            (response.status_code < 200 || response.status_code >= 300);
        if (err == ESP_ERR_NOT_SUPPORTED ||
            (!worker.decoded_any && err == ESP_OK) ||
            http_rejected) {
            webradio_set_playback_state(
                WEBRADIO_PLAYBACK_ERROR,
                err == ESP_ERR_NOT_SUPPORTED ? "URL is not an MP3 stream" :
                "stream returned no playable MP3 audio");
            break;
        }

        char reconnect_message[96];
        snprintf(reconnect_message,
                 sizeof(reconnect_message),
                 "connection ended (%s)",
                 esp_err_to_name(err));
        webradio_set_playback_state(WEBRADIO_PLAYBACK_RECONNECTING,
                                    reconnect_message);
        for (uint32_t waited = 0U;
             waited < WEBRADIO_RECONNECT_DELAY_MS && !webradio.stop_requested;
             waited += 50U) {
            vTaskDelay(pdMS_TO_TICKS(50U));
        }
    }
    webradio_worker_free(&worker);

done:
    webradio_request_publish(NULL);
    if (webradio.stop_requested) {
        webradio_set_playback_state(WEBRADIO_PLAYBACK_IDLE, NULL);
    }
    webradio.task_done = true;
    solar_os_task_delete_external(NULL);
}

static void webradio_stop_playback(void)
{
    webradio.stop_requested = true;
    solar_os_http_request_t *request = NULL;
    portENTER_CRITICAL(&webradio_lock);
    request = webradio.request;
    portEXIT_CRITICAL(&webradio_lock);
    if (request != NULL) {
        (void)solar_os_http_request_cancel(request);
    }
    if (webradio.task != NULL &&
        !solar_os_task_wait_done(webradio.task,
                                 &webradio.task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "radio task did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return;
    }
    webradio.task = NULL;
    webradio.task_done = true;
    webradio.stop_requested = false;
    webradio_set_playback_state(WEBRADIO_PLAYBACK_IDLE, NULL);
}

static esp_err_t webradio_start_playback(const char *name, const char *url)
{
    if (name == NULL || url == NULL || !solar_os_webradio_url_valid(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    webradio_stop_playback();
    portENTER_CRITICAL(&webradio_lock);
    strlcpy(webradio.active_name, name, sizeof(webradio.active_name));
    strlcpy(webradio.active_url, url, sizeof(webradio.active_url));
    webradio.source_rate = 0U;
    webradio.source_channels = 0U;
    webradio.stop_requested = false;
    webradio.task_done = false;
    webradio.redraw = true;
    portEXIT_CRITICAL(&webradio_lock);

    const BaseType_t created = solar_os_task_create_pinned_external(
        webradio_network_task,
        "webradio_net",
        WEBRADIO_TASK_STACK,
        NULL,
        WEBRADIO_TASK_PRIORITY,
        &webradio.task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        webradio.task = NULL;
        webradio.task_done = true;
        webradio_set_playback_state(WEBRADIO_PLAYBACK_ERROR,
                                    "task creation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void webradio_play_selected(void)
{
    if (webradio.cursor >= webradio.station_count) {
        return;
    }
    (void)webradio_start_playback(webradio.stations[webradio.cursor].name,
                                  webradio.stations[webradio.cursor].url);
}

static void webradio_remove_selected(void)
{
    if (webradio.cursor >= webradio.station_count) {
        return;
    }
    char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    webradio_snapshot_status(NULL,
                             NULL,
                             0U,
                             active_name,
                             sizeof(active_name),
                             NULL,
                             NULL,
                             NULL);
    if (strcmp(active_name, webradio.stations[webradio.cursor].name) == 0) {
        webradio_stop_playback();
        portENTER_CRITICAL(&webradio_lock);
        webradio.active_name[0] = '\0';
        webradio.active_url[0] = '\0';
        portEXIT_CRITICAL(&webradio_lock);
    }
    (void)solar_os_webradio_catalog_remove(
        webradio.stations[webradio.cursor].name);
    webradio_refresh_catalog();
    portENTER_CRITICAL(&webradio_lock);
    webradio.redraw = true;
    portEXIT_CRITICAL(&webradio_lock);
}

static bool webradio_manage_command(solar_os_context_t *ctx, esp_err_t *result)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc < 2) {
        return false;
    }
    const char *command = solar_os_context_argv(ctx, 1);
    solar_os_shell_io_t *io = webradio_io(ctx);
    esp_err_t err = ESP_OK;
    bool handled = true;
    if (strcmp(command, "list") == 0 && argc == 2) {
        solar_os_webradio_station_t stations[SOLAR_OS_WEBRADIO_STATION_MAX];
        const size_t count = solar_os_webradio_catalog_snapshot(
            stations, SOLAR_OS_WEBRADIO_STATION_MAX, NULL);
        for (size_t i = 0U; i < count; i++) {
            solar_os_shell_io_printf(io, "%s\t%s\n", stations[i].name, stations[i].url);
        }
        if (count == 0U) {
            solar_os_shell_io_writeln(io, "webradio: catalog is empty");
        }
    } else if (strcmp(command, "add") == 0 && argc == 4) {
        err = solar_os_webradio_catalog_add(solar_os_context_argv(ctx, 2),
                                            solar_os_context_argv(ctx, 3));
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "webradio: saved %s\n",
                                     solar_os_context_argv(ctx, 2));
        }
    } else if (strcmp(command, "remove") == 0 && argc == 3) {
        err = solar_os_webradio_catalog_remove(solar_os_context_argv(ctx, 2));
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "webradio: removed %s\n",
                                     solar_os_context_argv(ctx, 2));
        }
    } else if (strcmp(command, "reset") == 0 && argc == 2) {
        err = solar_os_webradio_catalog_reset();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(io, "webradio: restored default stations");
        }
    } else {
        handled = false;
    }

    if (!handled) {
        return false;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "webradio: %s\n",
                                 esp_err_to_name(err));
    }
    solar_os_shell_io_flush(io);
    solar_os_context_request_terminal_preserve(ctx);
    solar_os_context_request_exit(ctx);
    if (result != NULL) {
        *result = err;
    }
    return true;
}

static esp_err_t webradio_start(solar_os_context_t *ctx)
{
    memset(&webradio, 0, sizeof(webradio));
    webradio.task_done = true;
    esp_err_t err = solar_os_webradio_catalog_init();
    if (err != ESP_OK) {
        return err;
    }

    if (webradio_manage_command(ctx, &err)) {
        return ESP_OK;
    }
    const int argc = solar_os_context_argc(ctx);
    if (argc > 2 ||
        (argc == 2 &&
         !solar_os_webradio_url_valid(solar_os_context_argv(ctx, 1)))) {
        return ESP_ERR_INVALID_ARG;
    }

    webradio.mode = webradio_graphical_session(ctx) ?
        WEBRADIO_MODE_GRAPHICS : WEBRADIO_MODE_TUI;
    if (webradio.mode == WEBRADIO_MODE_TUI) {
        err = solar_os_tui_begin(&webradio.tui, ctx);
        if (err != ESP_OK) {
            return err;
        }
        (void)solar_os_tui_enable_diff(&webradio.tui, true);
    } else {
        solar_os_context_set_graphics_active(ctx, true);
    }
    webradio.ui_started = true;
    webradio_refresh_catalog();
    webradio.redraw = true;
    if (argc == 2) {
        err = webradio_start_playback("Direct stream",
                                      solar_os_context_argv(ctx, 1));
        if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
            webradio_set_playback_state(WEBRADIO_PLAYBACK_ERROR,
                                        esp_err_to_name(err));
        }
    }
    webradio_render(ctx);
    return ESP_OK;
}

static void webradio_stop(solar_os_context_t *ctx)
{
    webradio_stop_playback();
    if (!webradio.ui_started) {
        return;
    }
    if (webradio.mode == WEBRADIO_MODE_GRAPHICS) {
        solar_os_context_set_graphics_active(ctx, false);
    } else {
        solar_os_tui_set_cursor_visible(&webradio.tui, true);
        solar_os_tui_refresh(&webradio.tui);
        solar_os_tui_end(&webradio.tui);
        solar_os_context_request_terminal_preserve(ctx);
    }
    webradio.ui_started = false;
}

static void webradio_suspend(solar_os_context_t *ctx)
{
    webradio.suspended = true;
    if (webradio.mode == WEBRADIO_MODE_GRAPHICS) {
        solar_os_context_set_graphics_active(ctx, false);
    }
}

static void webradio_resume(solar_os_context_t *ctx)
{
    webradio.suspended = false;
    if (webradio.mode == WEBRADIO_MODE_GRAPHICS) {
        solar_os_context_set_graphics_active(ctx, true);
    }
    webradio.redraw = true;
    webradio_render(ctx);
}

static void webradio_title(solar_os_context_t *ctx,
                           char *buffer,
                           size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0U) {
        return;
    }
    char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    webradio_snapshot_status(NULL,
                             NULL,
                             0U,
                             active_name,
                             sizeof(active_name),
                             NULL,
                             NULL,
                             NULL);
    if (active_name[0] != '\0') {
        snprintf(buffer, buffer_len, "webradio: %s", active_name);
    } else {
        strlcpy(buffer, "webradio", buffer_len);
    }
}

static bool webradio_event(solar_os_context_t *ctx,
                           const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        webradio_resume(ctx);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        uint32_t generation = 0U;
        (void)solar_os_webradio_catalog_snapshot(NULL, 0U, &generation);
        if (generation != webradio.catalog_generation) {
            webradio_refresh_catalog();
            webradio.redraw = true;
        }
        bool redraw = false;
        webradio_snapshot_status(NULL,
                                 NULL,
                                 0U,
                                 NULL,
                                 0U,
                                 NULL,
                                 NULL,
                                 &redraw);
        if (redraw || webradio.redraw) {
            webradio_render(ctx);
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE ||
        key == 'q' || key == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }
    switch (key) {
    case SOLAR_OS_KEY_UP:
    case 'k':
        if (webradio.cursor > 0U) {
            webradio.cursor--;
        }
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        if (webradio.cursor + 1U < webradio.station_count) {
            webradio.cursor++;
        }
        break;
    case '\r':
    case '\n':
        webradio_play_selected();
        break;
    case ' ':
        webradio_stop_playback();
        break;
    case SOLAR_OS_KEY_DELETE:
    case 0x7f:
        webradio_remove_selected();
        break;
    case 'r':
    case 'R': {
        char name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
        char url[SOLAR_OS_WEBRADIO_URL_MAX];
        portENTER_CRITICAL(&webradio_lock);
        strlcpy(name, webradio.active_name, sizeof(name));
        strlcpy(url, webradio.active_url, sizeof(url));
        portEXIT_CRITICAL(&webradio_lock);
        if (url[0] != '\0') {
            (void)webradio_start_playback(name, url);
        }
        break;
    }
    default:
        return true;
    }
    webradio.redraw = true;
    webradio_render(ctx);
    return true;
}

const solar_os_app_t solar_os_webradio_app = {
    .name = "webradio",
    .summary = "streaming internet radio",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = webradio_start,
    .suspend = webradio_suspend,
    .resume = webradio_resume,
    .stop = webradio_stop,
    .event = webradio_event,
    .title = webradio_title,
    .worker_stack_bytes = WEBRADIO_TASK_STACK,
    .worker_stack_external = true,
};
