#include "solar_os_logic_app.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_gfx.h"
#include "solar_os_jobs.h"
#include "solar_os_keys.h"
#include "solar_os_logic.h"
#include "solar_os_memory.h"
#include "solar_os_appbar.h"

#define LOGIC_REFRESH_MS 250U

typedef struct {
    bool running;
    bool suspended;
    uint8_t *samples;
    size_t sample_capacity;
    solar_os_logic_status_t capture;
    size_t view_start;
    size_t visible_samples;
    uint32_t next_refresh_ms;
    char message[64];
} logic_app_state_t;

static void *logic_app_state;
#define logic_app (*(logic_app_state_t *)logic_app_state)

static bool logic_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static esp_err_t logic_parse_pins(const char *text, solar_os_logic_config_t *config)
{
    if (text == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char pins[SOLAR_OS_APP_ARG_LEN];
    strlcpy(pins, text, sizeof(pins));
    char *save = NULL;
    for (char *token = strtok_r(pins, ",", &save);
         token != NULL;
         token = strtok_r(NULL, ",", &save)) {
        uint32_t pin = 0;
        if (config->channel_count >= SOLAR_OS_LOGIC_MAX_CHANNELS ||
            !logic_parse_u32(token, 0, UINT8_MAX, &pin)) {
            return ESP_ERR_INVALID_ARG;
        }
        config->pins[config->channel_count++] = (uint8_t)pin;
    }
    return config->channel_count > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t logic_parse_config(solar_os_context_t *ctx, solar_os_logic_config_t *config)
{
    if (ctx == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int argc = solar_os_context_argc(ctx);
    if (argc < 2 || argc > 5) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    config->sample_rate_hz = SOLAR_OS_LOGIC_DEFAULT_RATE_HZ;
    config->sample_count = SOLAR_OS_LOGIC_DEFAULT_SAMPLES;

    esp_err_t err = logic_parse_pins(solar_os_context_argv(ctx, 1), config);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t positional_count = 0;
    for (int i = 2; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (arg != NULL && strncmp(arg, "trigger=", 8) == 0) {
            uint32_t pin = 0;
            if (config->trigger_enabled ||
                !logic_parse_u32(arg + 8, 0, UINT8_MAX, &pin)) {
                return ESP_ERR_INVALID_ARG;
            }
            config->trigger_enabled = true;
            config->trigger_pin = (uint8_t)pin;
        } else if (positional_count == 0) {
            if (!logic_parse_u32(arg,
                                 SOLAR_OS_LOGIC_MIN_RATE_HZ,
                                 SOLAR_OS_LOGIC_MAX_RATE_HZ,
                                 &config->sample_rate_hz)) {
                return ESP_ERR_INVALID_ARG;
            }
            positional_count++;
        } else if (positional_count == 1) {
            if (!logic_parse_u32(arg,
                                 1U,
                                 SOLAR_OS_LOGIC_MAX_SAMPLES,
                                 &config->sample_count)) {
                return ESP_ERR_INVALID_ARG;
            }
            positional_count++;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return solar_os_logic_validate_config(config);
}

static bool logic_sump_running(void)
{
    solar_os_job_status_t status;
    return solar_os_jobs_get_by_name("sump", &status) && status.state == SOLAR_OS_JOB_RUNNING;
}

static void logic_free_samples(void)
{
    solar_os_memory_free(logic_app.samples);
    logic_app.samples = NULL;
    logic_app.sample_capacity = 0;
}

static esp_err_t logic_reserve_samples(size_t count)
{
    if (logic_app.samples != NULL && logic_app.sample_capacity >= count) {
        return ESP_OK;
    }

    uint8_t *replacement = solar_os_memory_alloc(count,
                                                  SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                  "logic.samples");
    if (replacement == NULL) {
        return ESP_ERR_NO_MEM;
    }
    solar_os_memory_free(logic_app.samples);
    logic_app.samples = replacement;
    logic_app.sample_capacity = count;
    return ESP_OK;
}

static esp_err_t logic_load_latest(void)
{
    solar_os_logic_status_t status;
    esp_err_t err = solar_os_logic_get_status(&status);
    if (err != ESP_OK || !status.has_capture) {
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    err = logic_reserve_samples(status.config.sample_count);
    if (err != ESP_OK) {
        return err;
    }

    size_t copied = 0;
    err = solar_os_logic_copy_samples(0,
                                      logic_app.samples,
                                      status.config.sample_count,
                                      &copied);
    if (err != ESP_OK || copied != status.config.sample_count) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }

    const bool first_capture = !logic_app.capture.has_capture;
    logic_app.capture = status;
    if (first_capture || logic_app.visible_samples == 0) {
        logic_app.visible_samples = status.config.sample_count;
        logic_app.view_start = 0;
    } else {
        if (logic_app.visible_samples > status.config.sample_count) {
            logic_app.visible_samples = status.config.sample_count;
        }
        if (logic_app.view_start + logic_app.visible_samples > status.config.sample_count) {
            logic_app.view_start = status.config.sample_count - logic_app.visible_samples;
        }
    }
    logic_app.message[0] = '\0';
    return ESP_OK;
}

static int logic_level_y(int row_top, int row_height, bool high)
{
    const int pad = row_height > 8 ? 3 : 1;
    return high ? row_top + pad : row_top + row_height - pad - 1;
}

static void logic_draw_channel(solar_os_gfx_t *gfx,
                               uint8_t channel,
                               int left,
                               int top,
                               int width,
                               int height)
{
    const size_t count = logic_app.capture.config.sample_count;
    if (count == 0 || logic_app.visible_samples == 0 || width <= 1 || height <= 3) {
        return;
    }

    char label[12];
    snprintf(label, sizeof(label), "G%u", (unsigned)logic_app.capture.config.pins[channel]);
    solar_os_gfx_text(gfx, 2, top + (height / 2) + 3, label);

    size_t sample_index = logic_app.view_start;
    bool level = (logic_app.samples[sample_index] & (1U << channel)) != 0;
    int previous_x = left;
    int previous_y = logic_level_y(top, height, level);

    for (int x = 1; x < width; x++) {
        sample_index = logic_app.view_start +
            ((size_t)x * (logic_app.visible_samples - 1U)) / (size_t)(width - 1);
        if (sample_index >= count) {
            sample_index = count - 1U;
        }
        const bool next_level = (logic_app.samples[sample_index] & (1U << channel)) != 0;
        const int screen_x = left + x;
        const int next_y = logic_level_y(top, height, next_level);
        solar_os_gfx_line(gfx, previous_x, previous_y, screen_x, previous_y);
        if (next_y != previous_y) {
            solar_os_gfx_line(gfx, screen_x, previous_y, screen_x, next_y);
        }
        previous_x = screen_x;
        previous_y = next_y;
    }
}

/* Builds the footer chips (only meaningful once a capture exists). Same set
 * feeds drawing and click hit-testing so they never disagree. */
static size_t logic_build_footer(solar_os_appbar_shortcut_t *items, size_t max_items)
{
    size_t n = 0;
    if (logic_app.capture.has_capture) {
        if (n < max_items) { items[n].key = '+'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Zoom+"); n++; }
        if (n < max_items) { items[n].key = '-'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Zoom-"); n++; }
        if (n < max_items) { items[n].key = 'a'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Fit"); n++; }
    }
    /* Capture is always available so a failed/absent capture can be retried. */
    if (n < max_items) { items[n].key = 'r'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Capture"); n++; }
    return n;
}

static void logic_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || logic_app.suspended) {
        return;
    }

    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* Shared header bar; capture summary rides in the status line. */
    solar_os_appbar_header_t header_bar = {0};
    header_bar.title = "Logic Analyzer";
    header_bar.show_back = true;
    char status_line[96];
    if (logic_app.capture.has_capture) {
        snprintf(status_line, sizeof(status_line),
                 "%uch  %luHz  %lu smp",
                 (unsigned)logic_app.capture.config.channel_count,
                 (unsigned long)logic_app.capture.effective_rate_hz,
                 (unsigned long)logic_app.capture.config.sample_count);
        if (logic_app.capture.config.trigger_enabled) {
            char trigger[16];
            snprintf(trigger, sizeof(trigger), "  trigG%u",
                     (unsigned)logic_app.capture.config.trigger_pin);
            strlcat(status_line, trigger, sizeof(status_line));
        }
        header_bar.status_line = status_line;
    }
    solar_os_appbar_draw_header(gfx, &header_bar);

    const int hh = solar_os_appbar_header_height(gfx);
    const int sh = header_bar.status_line ? solar_os_appbar_status_line_height(gfx) : 0;
    const int fh = solar_os_appbar_footer_height(gfx);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    if (!logic_app.capture.has_capture) {
        solar_os_gfx_text(gfx, 4, hh + (height - hh) / 2,
                          logic_app.message[0] ? logic_app.message : "No capture yet");
        solar_os_gfx_present(gfx);
        return;
    }

    const int left = width > 120 ? 32 : 22;
    const int top = hh + sh + 4;
    const int win_line = 12;               /* sample-window info above the footer */
    const int bottom = fh + win_line + 2;
    const int plot_height = height - top - bottom;
    const int row_height = plot_height / logic_app.capture.config.channel_count;
    const int plot_width = width - left - 2;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
    for (int i = 0; i <= 4; i++) {
        const int x = left + (plot_width * i) / 4;
        solar_os_gfx_line(gfx, x, top, x, top + plot_height - 1);
    }
    for (uint8_t channel = 0; channel < logic_app.capture.config.channel_count; channel++) {
        const int y = top + channel * row_height;
        solar_os_gfx_line(gfx, left, y + row_height - 1, left + plot_width, y + row_height - 1);
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    for (uint8_t channel = 0; channel < logic_app.capture.config.channel_count; channel++) {
        logic_draw_channel(gfx,
                           channel,
                           left,
                           top + channel * row_height,
                           plot_width,
                           row_height);
    }

    const size_t end = logic_app.view_start + logic_app.visible_samples;
    const uint64_t start_us =
        ((uint64_t)logic_app.view_start * 1000000ULL) / logic_app.capture.effective_rate_hz;
    const uint64_t span_us =
        ((uint64_t)logic_app.visible_samples * 1000000ULL) / logic_app.capture.effective_rate_hz;
    char winfo[96];
    snprintf(winfo,
             sizeof(winfo),
             "%u-%u  +%lluus  span %lluus%s%s",
             (unsigned)(logic_app.view_start + 1U),
             (unsigned)end,
             (unsigned long long)start_us,
             (unsigned long long)span_us,
             logic_app.message[0] ? "  " : "",
             logic_app.message);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 2, height - fh - 3, winfo);

    /* Shared footer chips (Zoom/Fit/Capture; pan is drag or arrow keys). */
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = logic_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);

    solar_os_gfx_present(gfx);
}

static esp_err_t logic_capture_local(const solar_os_logic_config_t *config)
{
    if (logic_sump_running()) {
        strlcpy(logic_app.message, "SUMP owns capture", sizeof(logic_app.message));
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = solar_os_logic_capture(config);
    if (err != ESP_OK) {
        snprintf(logic_app.message, sizeof(logic_app.message), "%s", esp_err_to_name(err));
        return err;
    }
    return logic_load_latest();
}

/* Runs a capture, reusing the current config or falling back to the default
 * one when no capture exists yet. Used by the 'r' key and the Capture chip. */
static void logic_do_capture(void)
{
    solar_os_logic_config_t config;
    if (logic_app.capture.has_capture) {
        config = logic_app.capture.config;
    } else if (solar_os_logic_default_config(&config) != ESP_OK) {
        return;
    }
    (void)logic_capture_local(&config);
}

static esp_err_t logic_start(solar_os_context_t *ctx)
{
    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&logic_app, 0, sizeof(logic_app));
    esp_err_t err = solar_os_logic_init();
    if (err != ESP_OK) {
        return err;
    }

    if (solar_os_context_argc(ctx) > 1) {
        solar_os_logic_config_t config;
        err = logic_parse_config(ctx, &config);
        if (err == ESP_OK) {
            err = logic_capture_local(&config);
        }
    } else {
        err = logic_load_latest();
        if (err == ESP_ERR_NOT_FOUND && !logic_sump_running()) {
            solar_os_logic_config_t config;
            err = solar_os_logic_default_config(&config);
            if (err == ESP_OK) {
                err = logic_capture_local(&config);
            }
        }
    }

    /* A failed or absent capture must NOT close the app -- open anyway and
     * let the user retry with the Capture chip. Keep any message already set
     * by logic_capture_local; otherwise explain the empty state. */
    if (err == ESP_ERR_NOT_FOUND) {
        strlcpy(logic_app.message, "No capture yet -- tap Capture", sizeof(logic_app.message));
    } else if (err != ESP_OK && logic_app.message[0] == '\0') {
        snprintf(logic_app.message, sizeof(logic_app.message), "Capture failed: %s",
                 esp_err_to_name(err));
    }

    logic_app.running = true;
    solar_os_context_set_graphics_active(ctx, true);
    logic_render(ctx);
    return ESP_OK;
}

static void logic_stop(solar_os_context_t *ctx)
{
    logic_app.running = false;
    logic_app.suspended = false;
    logic_free_samples();
    solar_os_context_set_graphics_active(ctx, false);
}

static void logic_suspend(solar_os_context_t *ctx)
{
    logic_app.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void logic_resume(solar_os_context_t *ctx)
{
    logic_app.suspended = false;
    solar_os_context_set_graphics_active(ctx, true);
    logic_render(ctx);
}

static void logic_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len > 0) {
        strlcpy(buffer, "logic", buffer_len);
    }
}

static void logic_zoom(bool in)
{
    const size_t total = logic_app.capture.config.sample_count;
    if (total == 0) {
        return;
    }
    const size_t center = logic_app.view_start + logic_app.visible_samples / 2U;
    size_t visible = in ? logic_app.visible_samples / 2U : logic_app.visible_samples * 2U;
    if (visible < 16U) {
        visible = total < 16U ? total : 16U;
    }
    if (visible > total) {
        visible = total;
    }
    logic_app.visible_samples = visible;
    logic_app.view_start = center > visible / 2U ? center - visible / 2U : 0;
    if (logic_app.view_start + visible > total) {
        logic_app.view_start = total - visible;
    }
}

static void logic_pan(bool right)
{
    const size_t total = logic_app.capture.config.sample_count;
    const size_t step = logic_app.visible_samples > 8U ? logic_app.visible_samples / 4U : 1U;
    if (right) {
        logic_app.view_start = logic_app.view_start + logic_app.visible_samples + step >= total ?
            total - logic_app.visible_samples :
            logic_app.view_start + step;
    } else {
        logic_app.view_start = logic_app.view_start > step ? logic_app.view_start - step : 0;
    }
}

/* Shifts the view window by a signed number of samples, clamped to bounds. */
static void logic_pan_samples(int delta)
{
    const size_t total = logic_app.capture.config.sample_count;
    if (total == 0 || logic_app.visible_samples >= total) {
        return;
    }
    const size_t max_start = total - logic_app.visible_samples;
    if (delta < 0) {
        const size_t mag = (size_t)(-delta);
        logic_app.view_start = logic_app.view_start > mag ? logic_app.view_start - mag : 0;
    } else {
        logic_app.view_start += (size_t)delta;
        if (logic_app.view_start > max_start) {
            logic_app.view_start = max_start;
        }
    }
}

static bool logic_handle_char(solar_os_context_t *ctx, char ch)
{
    const uint8_t key = (uint8_t)ch;
    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }

    /* Capture works even with no prior data (starts one from the default). */
    if (ch == 'r' || ch == 'R') {
        logic_do_capture();
        logic_render(ctx);
        return true;
    }

    if (!logic_app.capture.has_capture) {
        return false;
    }
    if (key == SOLAR_OS_KEY_LEFT) {
        logic_pan(false);
    } else if (key == SOLAR_OS_KEY_RIGHT) {
        logic_pan(true);
    } else if (key == SOLAR_OS_KEY_PAGE_UP || ch == '+' || ch == '=') {
        logic_zoom(true);
    } else if (key == SOLAR_OS_KEY_PAGE_DOWN || ch == '-') {
        logic_zoom(false);
    } else if (ch == 'a' || ch == 'A' || key == SOLAR_OS_KEY_HOME) {
        logic_app.view_start = 0;
        logic_app.visible_samples = logic_app.capture.config.sample_count;
    } else {
        return false;
    }

    logic_render(ctx);
    return true;
}

static bool logic_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return logic_handle_char(ctx, event->data.ch);
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        solar_os_appbar_header_t header = {0};
        header.show_back = true;
        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, px, py, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                solar_os_context_request_exit(ctx);
            }
            return true;
        }

        solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
        const size_t count = logic_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < count) {
                switch (items[fhit.index].key) {
                case '+': logic_zoom(true); break;
                case '-': logic_zoom(false); break;
                case 'a': logic_app.view_start = 0;                    /* Fit */
                          logic_app.visible_samples = logic_app.capture.config.sample_count;
                          break;
                case 'r': logic_do_capture(); break;
                default: break;
                }
                logic_render(ctx);
            }
            return true;
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_DRAG) {
        if (!logic_app.capture.has_capture) return true;
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int width = (int)solar_os_gfx_width(gfx);
        const int left = width > 120 ? 32 : 22;
        const int plot_width = width - left - 2;
        if (event->data.drag.dx != 0 && plot_width > 1) {
            /* Drag content: finger right (dx>0) scrolls the window left. */
            int delta = -(int)(((long)event->data.drag.dx *
                                (long)logic_app.visible_samples) / plot_width);
            if (delta == 0) delta = event->data.drag.dx > 0 ? -1 : 1;
            logic_pan_samples(delta);
            logic_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_SCROLL) {
        if (!logic_app.capture.has_capture) return true;
        logic_zoom(event->data.scroll.delta > 0);
        logic_render(ctx);
        return true;
    }

    if (event->type != SOLAR_OS_EVENT_TICK || logic_app.suspended) {
        return false;
    }
    if (logic_app.next_refresh_ms != 0 &&
        (int32_t)(event->data.tick_ms - logic_app.next_refresh_ms) < 0) {
        return false;
    }
    logic_app.next_refresh_ms = event->data.tick_ms + LOGIC_REFRESH_MS;

    solar_os_logic_status_t status;
    if (solar_os_logic_get_status(&status) == ESP_OK &&
        status.has_capture &&
        status.generation != logic_app.capture.generation &&
        logic_load_latest() == ESP_OK) {
        logic_render(ctx);
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_logic_app = {
    .name = "logic",
    .summary = "logic analyzer waveform viewer",
    .flags = 0,
    .start = logic_start,
    .suspend = logic_suspend,
    .resume = logic_resume,
    .stop = logic_stop,
    .event = logic_event,
    .title = logic_title,
    .state_slot = &logic_app_state,
    .state_size = sizeof(logic_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
};
