#include "solar_os_yazici.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

#define TAG "yazici"

#define YAZICI_MAX_TEXT_BYTES (64U * 1024U)
#define YAZICI_MAX_LINES 3072U
#define YAZICI_WORD_SCRATCH 256U
#define YAZICI_PROMPT_MAX 48U
#define YAZICI_STATUS_MAX 64U

#define YAZICI_FONT SOLAR_OS_GFX_FONT_MONO_16
#define YAZICI_LINE_H_SINGLE 22
#define YAZICI_LINE_H_DOUBLE 32
#define YAZICI_BASELINE_OFS 16
#define YAZICI_HEADER_H 24
#define YAZICI_FOOTER_H 22

#define YAZICI_TICK_MS 100U
#define YAZICI_BLINK_MS 500U
#define YAZICI_MARGIN_KEY_STEP 8

#define YAZICI_STRIKE_MARKER ((char)0x01)

#define YAZICI_TONE_CLICK_HZ 1900U
#define YAZICI_TONE_CLICK_MS 10U
#define YAZICI_TONE_RETURN_HZ 320U
#define YAZICI_TONE_RETURN_MS 45U
#define YAZICI_TONE_BELL_HZ 2600U
#define YAZICI_TONE_BELL_MS 90U
#define YAZICI_TONE_CORRECT_HZ 650U
#define YAZICI_TONE_CORRECT_MS 16U
#define YAZICI_BELL_MARGIN_CHARS 6

#define YAZICI_SETTINGS_DIR ".yazici"
#define YAZICI_SETTINGS_FILE "settings.cfg"
#define YAZICI_SETTINGS_MAGIC 0x595A4331U
#define YAZICI_SETTINGS_VERSION 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    int margin_left;
    int margin_right;
    uint32_t line_spacing;
    uint32_t sound_enabled;
    uint32_t classic_mode;
    uint32_t autosave_interval_ms;
} yazici_settings_t;

typedef struct {
    size_t start;
    size_t end;
} yazici_line_t;

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    size_t cursor;

    yazici_line_t lines[YAZICI_MAX_LINES];
    size_t line_count;
    size_t cursor_line;
    size_t scroll_top_line;

    int margin_left;
    int margin_right;
    bool margins_ready;
    uint32_t line_spacing;

    bool sound_enabled;
    bool classic_mode;
    bool last_char_was_bell;
    bool bell_check_pending;

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    bool has_path;
    bool dirty;
    uint32_t autosave_interval_ms;
    uint32_t last_autosave_ms;

    bool prompt_active;
    char prompt_buffer[YAZICI_PROMPT_MAX];
    size_t prompt_len;

    uint32_t elapsed_ms;
    uint32_t last_input_ms;
    uint32_t blink_accum_ms;
    bool blink_visible;
    bool render_pending;
    bool layout_pending;

    char status_message[YAZICI_STATUS_MAX];
    uint32_t status_until_ms;

    int page_number;
} yazici_state_t;

static void *yazici_state_ptr;
#define yazici (*(yazici_state_t *)yazici_state_ptr)

static void yazici_render(solar_os_context_t *ctx);
static void yazici_relayout(solar_os_gfx_t *gfx);
static void yazici_set_status(const char *message);
static bool yazici_save_now(void);

static void ensure_dir_exists(const char *path)
{
    char tmp[128];
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static size_t yazici_utf8_prev(const char *buf, size_t len, size_t pos)
{
    (void)len;
    if (pos == 0) return 0;
    size_t p = pos - 1;
    while (p > 0 && ((unsigned char)buf[p] & 0xC0) == 0x80) {
        p--;
    }
    if (p > 0 && (unsigned char)buf[p - 1] == (unsigned char)YAZICI_STRIKE_MARKER) {
        p--;
    }
    return p;
}

static size_t yazici_utf8_next(const char *buf, size_t len, size_t pos)
{
    if (pos >= len) return len;
    size_t p = pos;
    if ((unsigned char)buf[p] == (unsigned char)YAZICI_STRIKE_MARKER && p + 1 < len) {
        p++;
    }
    p++;
    while (p < len && ((unsigned char)buf[p] & 0xC0) == 0x80) {
        p++;
    }
    return p;
}

static void yazici_play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!yazici.sound_enabled) return;
    const solar_os_audio_tone_step_t step = {
        .frequency_hz = freq_hz,
        .duration_ms = duration_ms,
        .pause_ms = 0,
    };
    const solar_os_audio_tone_request_t request = {
        .steps = &step,
        .step_count = 1,
        .volume = 75,
        .drop_if_busy = true,
    };
    uint32_t request_id = 0;
    (void)solar_os_audio_tone_enqueue(&request, &request_id);
}

static bool yazici_ensure_capacity(size_t needed)
{
    if (needed <= yazici.capacity) return true;
    size_t new_cap = yazici.capacity == 0 ? 4096U : yazici.capacity;
    while (new_cap < needed && new_cap < YAZICI_MAX_TEXT_BYTES) {
        new_cap *= 2U;
    }
    if (new_cap > YAZICI_MAX_TEXT_BYTES) new_cap = YAZICI_MAX_TEXT_BYTES;
    if (new_cap < needed) return false;

    char *fresh = (char *)solar_os_memory_alloc(new_cap, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "yazici.buf");
    if (fresh == NULL) return false;

    if (yazici.buffer != NULL && yazici.length > 0U) {
        memcpy(fresh, yazici.buffer, yazici.length);
    }
    if (yazici.buffer != NULL) {
        solar_os_memory_free(yazici.buffer);
    }
    yazici.buffer = fresh;
    yazici.capacity = new_cap;
    return true;
}

static bool yazici_insert_at(size_t offset, const char *text, size_t len)
{
    if (len == 0U) return true;
    if (yazici.length + len > YAZICI_MAX_TEXT_BYTES) {
        yazici_set_status("Document buffer limit reached");
        return false;
    }
    if (!yazici_ensure_capacity(yazici.length + len)) {
        yazici_set_status("Out of memory");
        return false;
    }
    memmove(yazici.buffer + offset + len, yazici.buffer + offset, yazici.length - offset);
    memcpy(yazici.buffer + offset, text, len);
    yazici.length += len;
    if (yazici.cursor >= offset) {
        yazici.cursor += len;
    }
    yazici.dirty = true;
    yazici.layout_pending = true;
    yazici.render_pending = true;
    return true;
}

static void yazici_insert_text(const char *text, size_t len)
{
    (void)yazici_insert_at(yazici.cursor, text, len);
}

static void yazici_backspace_normal(void)
{
    if (yazici.cursor == 0 || yazici.length == 0) return;
    const size_t prev = yazici_utf8_prev(yazici.buffer, yazici.length, yazici.cursor);
    const size_t erase_len = yazici.cursor - prev;
    memmove(yazici.buffer + prev, yazici.buffer + yazici.cursor, yazici.length - yazici.cursor);
    yazici.length -= erase_len;
    yazici.cursor = prev;
    yazici.dirty = true;
    yazici.layout_pending = true;
    yazici.render_pending = true;
    yazici_play_tone(YAZICI_TONE_CLICK_HZ, YAZICI_TONE_CLICK_MS);
}

static void yazici_backspace_classic(void)
{
    if (yazici.cursor == 0) return;
    const size_t prev = yazici_utf8_prev(yazici.buffer, yazici.length, yazici.cursor);
    if (prev < yazici.length && (unsigned char)yazici.buffer[prev] == (unsigned char)YAZICI_STRIKE_MARKER) {
        yazici.cursor = prev;
        yazici.render_pending = true;
        yazici_play_tone(YAZICI_TONE_CORRECT_HZ, YAZICI_TONE_CORRECT_MS);
        return;
    }
    const char marker = YAZICI_STRIKE_MARKER;
    (void)yazici_insert_at(prev, &marker, 1U);
    yazici.cursor = prev;
    yazici.dirty = true;
    yazici.layout_pending = true;
    yazici.render_pending = true;
    yazici_play_tone(YAZICI_TONE_CORRECT_HZ, YAZICI_TONE_CORRECT_MS);
}

static void yazici_delete_forward(void)
{
    if (yazici.cursor >= yazici.length) return;
    const size_t next = yazici_utf8_next(yazici.buffer, yazici.length, yazici.cursor);
    const size_t erase_len = next - yazici.cursor;
    memmove(yazici.buffer + yazici.cursor, yazici.buffer + next, yazici.length - next);
    yazici.length -= erase_len;
    yazici.dirty = true;
    yazici.layout_pending = true;
    yazici.render_pending = true;
}

static int yazici_measure(solar_os_gfx_t *gfx, const char *text, size_t len)
{
    char scratch[YAZICI_WORD_SCRATCH];
    size_t out = 0U;
    for (size_t i = 0U; i < len && out + 1U < sizeof(scratch); i++) {
        if (text[i] == YAZICI_STRIKE_MARKER) continue;
        scratch[out++] = text[i];
    }
    scratch[out] = '\0';
    solar_os_gfx_set_font(gfx, YAZICI_FONT);
    return (int)solar_os_gfx_text_width(gfx, scratch);
}

static void yazici_maybe_ring_bell(void)
{
    if (yazici.line_count == 0U) return;
    const yazici_line_t *cur = &yazici.lines[yazici.cursor_line];
    size_t len_in_chars = 0U;
    for (size_t i = cur->start; i < cur->end; i++) {
        if (yazici.buffer[i] != YAZICI_STRIKE_MARKER && ((unsigned char)yazici.buffer[i] & 0xC0) != 0x80) {
            len_in_chars++;
        }
    }
    if (len_in_chars >= 50 && !yazici.last_char_was_bell) {
        yazici_play_tone(YAZICI_TONE_BELL_HZ, YAZICI_TONE_BELL_MS);
        yazici.last_char_was_bell = true;
    } else if (len_in_chars < 50) {
        yazici.last_char_was_bell = false;
    }
}

static void yazici_relayout(solar_os_gfx_t *gfx)
{
    if (gfx == NULL || !yazici.margins_ready) {
        yazici.line_count = 0U;
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int max_width = width - yazici.margin_left - yazici.margin_right;
    solar_os_gfx_set_font(gfx, YAZICI_FONT);

    size_t count = 0U;
    size_t pos = 0U;
    bool prev_hard_break = true;

    while (pos < yazici.length && count < YAZICI_MAX_LINES) {
        size_t line_start = pos;
        if (!prev_hard_break) {
            while (line_start < yazici.length && yazici.buffer[line_start] == ' ') {
                line_start++;
            }
        }
        pos = line_start;
        size_t line_end = pos;
        int width_used = 0;
        bool has_content = false;
        prev_hard_break = false;

        for (;;) {
            if (pos >= yazici.length) {
                line_end = pos;
                break;
            }
            const unsigned char c = (unsigned char)yazici.buffer[pos];
            if (c == '\n' || c == '\f') {
                line_end = pos;
                pos += 1U;
                prev_hard_break = true;
                break;
            }
            size_t word_start = pos;
            size_t word_end = pos;
            if (c == ' ') {
                while (word_end < yazici.length && yazici.buffer[word_end] == ' ') {
                    word_end++;
                }
            } else {
                while (word_end < yazici.length) {
                    const unsigned char wc = (unsigned char)yazici.buffer[word_end];
                    if (wc == ' ' || wc == '\n' || wc == '\f') break;
                    word_end++;
                }
            }
            const int word_width = yazici_measure(gfx, yazici.buffer + word_start, word_end - word_start);

            if (has_content && (width_used + word_width) > max_width) {
                line_end = pos;
                break;
            }
            if (!has_content && word_width > max_width && word_end > word_start) {
                size_t cut = word_start;
                size_t last_good = word_start;
                while (cut < word_end) {
                    const size_t next = yazici_utf8_next(yazici.buffer, yazici.length, cut);
                    const int w = yazici_measure(gfx, yazici.buffer + word_start, next - word_start);
                    if (w > max_width && next > yazici_utf8_next(yazici.buffer, yazici.length, word_start)) break;
                    last_good = next;
                    cut = next;
                }
                if (last_good == word_start) {
                    last_good = yazici_utf8_next(yazici.buffer, yazici.length, word_start);
                }
                line_end = last_good;
                pos = last_good;
                has_content = true;
                break;
            }
            width_used += word_width;
            line_end = word_end;
            pos = word_end;
            has_content = true;
        }

        yazici.lines[count].start = line_start;
        yazici.lines[count].end = line_end;
        count++;
    }

    if (yazici.length == 0U && count == 0U) {
        yazici.lines[0].start = 0U;
        yazici.lines[0].end = 0U;
        count = 1U;
    } else if (yazici.length > 0U && count < YAZICI_MAX_LINES) {
        const unsigned char last = (unsigned char)yazici.buffer[yazici.length - 1U];
        if (last == '\n' || last == '\f') {
            yazici.lines[count].start = yazici.length;
            yazici.lines[count].end = yazici.length;
            count++;
        }
    }

    yazici.line_count = count;
    yazici.layout_pending = false;

    size_t cursor_line = 0U;
    for (size_t i = 0U; i < count; i++) {
        if (yazici.cursor >= yazici.lines[i].start && yazici.cursor <= yazici.lines[i].end) {
            cursor_line = i;
            if (yazici.cursor < yazici.lines[i].end || i + 1U == count) break;
        }
    }
    yazici.cursor_line = cursor_line;

    int page = 1;
    for (size_t i = 0U; i < yazici.cursor && i < yazici.length; i++) {
        if ((unsigned char)yazici.buffer[i] == '\f') page++;
    }
    yazici.page_number = page;

    if (yazici.bell_check_pending) {
        yazici.bell_check_pending = false;
        yazici_maybe_ring_bell();
    }
}

static int yazici_visible_line_count(solar_os_gfx_t *gfx)
{
    const int height = (int)solar_os_gfx_height(gfx);
    const int body_h = height - YAZICI_HEADER_H - YAZICI_FOOTER_H;
    const int line_h = yazici.line_spacing >= 2U ? YAZICI_LINE_H_DOUBLE : YAZICI_LINE_H_SINGLE;
    int count = body_h / line_h;
    return count < 1 ? 1 : count;
}

static void yazici_ensure_cursor_visible(solar_os_gfx_t *gfx)
{
    const int visible = yazici_visible_line_count(gfx);
    if (yazici.cursor_line < yazici.scroll_top_line) {
        yazici.scroll_top_line = yazici.cursor_line;
    } else if (yazici.cursor_line >= yazici.scroll_top_line + (size_t)visible) {
        yazici.scroll_top_line = yazici.cursor_line - (size_t)visible + 1U;
    }
}

/* ---------------------------------------------------------------------
 * Settings and File IO
 * ------------------------------------------------------------------- */

static void yazici_apply_default_settings(void)
{
    yazici.margin_left = -1;
    yazici.margin_right = -1;
    yazici.margins_ready = false;
    yazici.line_spacing = 1U;
    yazici.sound_enabled = true;
    yazici.classic_mode = false;
    yazici.autosave_interval_ms = 20000U;
}

static void yazici_load_settings(void)
{
    yazici_apply_default_settings();
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    /* Settings live on the SD card exclusively (solar_os_storage_app_data_path);
     * no SD card just means "use defaults", same as no settings file yet. */
    if (solar_os_storage_app_data_path("yazici", YAZICI_SETTINGS_FILE, path, sizeof(path)) != ESP_OK) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) return;
    yazici_settings_t settings = {0};
    const size_t read = fread(&settings, 1, sizeof(settings), f);
    fclose(f);
    if (read != sizeof(settings) || settings.magic != YAZICI_SETTINGS_MAGIC || settings.version != YAZICI_SETTINGS_VERSION) {
        return;
    }
    if (settings.margin_left >= 0) yazici.margin_left = settings.margin_left;
    if (settings.margin_right >= 0) yazici.margin_right = settings.margin_right;
    yazici.margins_ready = (settings.margin_left >= 0 && settings.margin_right >= 0);
    yazici.line_spacing = settings.line_spacing == 2U ? 2U : 1U;
    yazici.sound_enabled = settings.sound_enabled != 0U;
    yazici.classic_mode = settings.classic_mode != 0U;
    if (settings.autosave_interval_ms >= 2000U) {
        yazici.autosave_interval_ms = settings.autosave_interval_ms;
    }
}

static void yazici_save_settings(void)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    const esp_err_t path_err = solar_os_storage_app_data_path("yazici", YAZICI_SETTINGS_FILE, path, sizeof(path));
    if (path_err == SOLAR_OS_STORAGE_ERR_NO_SD_CARD) {
        yazici_set_status("No SD card - settings not saved");
        return;
    }
    if (path_err != ESP_OK) return;

    const yazici_settings_t settings = {
        .magic = YAZICI_SETTINGS_MAGIC,
        .version = YAZICI_SETTINGS_VERSION,
        .margin_left = yazici.margin_left,
        .margin_right = yazici.margin_right,
        .line_spacing = yazici.line_spacing,
        .sound_enabled = yazici.sound_enabled ? 1U : 0U,
        .classic_mode = yazici.classic_mode ? 1U : 0U,
        .autosave_interval_ms = yazici.autosave_interval_ms,
    };
    FILE *f = fopen(path, "wb");
    if (f == NULL) return;
    (void)fwrite(&settings, 1, sizeof(settings), f);
    fflush(f);
    fclose(f);
}

static void yazici_set_status(const char *message)
{
    strncpy(yazici.status_message, message, sizeof(yazici.status_message) - 1U);
    yazici.status_message[sizeof(yazici.status_message) - 1U] = '\0';
    yazici.status_until_ms = yazici.elapsed_ms + 2500U;
    yazici.render_pending = true;
}

static bool yazici_save_now(void)
{
    if (!yazici.has_path || yazici.path[0] == '\0') {
        const char *mount = solar_os_storage_sd_is_mounted() ?
            solar_os_storage_sd_mount_point() :
            (solar_os_storage_flash_is_mounted() ? solar_os_storage_flash_mount_point() : "/sdcard");
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/doc", mount);
        ensure_dir_exists(dir);
        snprintf(yazici.path, sizeof(yazici.path), "%s/typewriter.txt", dir);
        yazici.has_path = true;
    }

    char parent_dir[SOLAR_OS_STORAGE_PATH_MAX];
    strlcpy(parent_dir, yazici.path, sizeof(parent_dir));
    char *last_slash = strrchr(parent_dir, '/');
    if (last_slash != NULL) {
        *last_slash = '\0';
        ensure_dir_exists(parent_dir);
    }

    FILE *f = fopen(yazici.path, "wb");
    if (f == NULL) {
        yazici_set_status("Save Failed: File Open Error");
        SOLAR_OS_LOGW(TAG, "save failed to open %s", yazici.path);
        return false;
    }
    const size_t written = fwrite(yazici.buffer, 1, yazici.length, f);
    const int flush_ok = fflush(f);
    fclose(f);

    if (written != yazici.length || flush_ok != 0) {
        yazici_set_status("Save Failed: Write Error");
        return false;
    }

    yazici.dirty = false;
    yazici.last_autosave_ms = yazici.elapsed_ms;
    char msg[64];
    const char *basename = strrchr(yazici.path, '/');
    snprintf(msg, sizeof(msg), "Saved: %s", basename != NULL ? basename + 1 : yazici.path);
    yazici_set_status(msg);
    return true;
}

static void yazici_load_file(const char *resolved_path)
{
    FILE *f = fopen(resolved_path, "rb");
    if (f == NULL) return;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return;
    }
    if ((size_t)size > YAZICI_MAX_TEXT_BYTES) size = (long)YAZICI_MAX_TEXT_BYTES;
    rewind(f);
    if (!yazici_ensure_capacity((size_t)size)) {
        fclose(f);
        return;
    }
    const size_t read = fread(yazici.buffer, 1, (size_t)size, f);
    fclose(f);
    yazici.length = read;
    yazici.cursor = 0U;
    yazici.dirty = false;
    yazici.layout_pending = true;
}

/* ---------------------------------------------------------------------
 * UI Rendering
 * ------------------------------------------------------------------- */

static void yazici_draw_strike(solar_os_gfx_t *gfx, int x, int baseline_y, int width)
{
    const int line_y = baseline_y - 5;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, x, line_y, x + width - 1, line_y);
    solar_os_gfx_line(gfx, x, line_y + 1, x + width - 1, line_y + 1);
}

static void yazici_draw_line_text(solar_os_gfx_t *gfx, int start_x, int baseline_y, const char *start, size_t len)
{
    int x = start_x;
    size_t i = 0U;
    while (i < len) {
        bool struck = false;
        if ((unsigned char)start[i] == (unsigned char)YAZICI_STRIKE_MARKER) {
            struck = true;
            i++;
            if (i >= len) break;
        }
        const size_t char_start = i;
        size_t char_end = i + 1U;
        while (char_end < len && ((unsigned char)start[char_end] & 0xC0) == 0x80) {
            char_end++;
        }
        char glyph[8];
        size_t glyph_len = char_end - char_start;
        if (glyph_len >= sizeof(glyph)) glyph_len = sizeof(glyph) - 1U;
        memcpy(glyph, start + char_start, glyph_len);
        glyph[glyph_len] = '\0';
        solar_os_gfx_text(gfx, x, baseline_y, glyph);
        const int glyph_w = (int)solar_os_gfx_text_width(gfx, glyph);
        if (struck) {
            yazici_draw_strike(gfx, x, baseline_y, glyph_w);
        }
        x += glyph_w;
        i = char_end;
    }
}

static int yazici_x_for_offset(solar_os_gfx_t *gfx, const yazici_line_t *line, size_t offset)
{
    if (offset <= line->start) return 0;
    size_t len = offset - line->start;
    if (offset > line->end) len = line->end - line->start;
    return yazici_measure(gfx, yazici.buffer + line->start, len);
}

/* Inverse of yazici_x_for_offset(): walks the line the same way
 * yazici_draw_line_text() draws it (respecting strike markers and UTF-8
 * continuation bytes) and returns the byte offset of whichever character
 * boundary is closest to target_x, for click-to-position-cursor. */
static size_t yazici_offset_for_x(solar_os_gfx_t *gfx, const yazici_line_t *line, int target_x)
{
    if (target_x <= 0) return line->start;
    solar_os_gfx_set_font(gfx, YAZICI_FONT);

    size_t i = line->start;
    int x = 0;
    while (i < line->end) {
        if ((unsigned char)yazici.buffer[i] == (unsigned char)YAZICI_STRIKE_MARKER) {
            i++;
            if (i >= line->end) break;
        }
        const size_t char_start = i;
        size_t char_end = char_start + 1U;
        while (char_end < line->end && ((unsigned char)yazici.buffer[char_end] & 0xC0) == 0x80) {
            char_end++;
        }
        char glyph[8];
        size_t glyph_len = char_end - char_start;
        if (glyph_len >= sizeof(glyph)) glyph_len = sizeof(glyph) - 1U;
        memcpy(glyph, yazici.buffer + char_start, glyph_len);
        glyph[glyph_len] = '\0';
        const int glyph_w = (int)solar_os_gfx_text_width(gfx, glyph);
        if (target_x < x + glyph_w / 2) {
            return char_start;
        }
        x += glyph_w;
        i = char_end;
    }
    return line->end;
}

static void yazici_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);

    if (!yazici.margins_ready) {
        yazici.margin_left = width / 12;
        yazici.margin_right = width / 12;
        yazici.margins_ready = true;
        yazici.layout_pending = true;
    }
    if (yazici.layout_pending) {
        yazici_relayout(gfx);
        yazici_ensure_cursor_visible(gfx);
    }

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, YAZICI_HEADER_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

    char header[96];
    const char *doc_name = yazici.has_path ? yazici.path : "typewriter.txt";
    const char *base = strrchr(doc_name, '/');
    snprintf(header, sizeof(header), "TYPEWRITER - %s%s | Page %d | %s",
             base != NULL ? base + 1 : doc_name,
             yazici.dirty ? " *" : "",
             yazici.page_number,
             yazici.sound_enabled ? "Sound ON" : "Muted");
    solar_os_gfx_text(gfx, 8, 16, header);

    /* Paper Margins */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, yazici.margin_left - 4, YAZICI_HEADER_H, yazici.margin_left - 4, height - YAZICI_FOOTER_H);
    solar_os_gfx_line(gfx, width - yazici.margin_right + 4, YAZICI_HEADER_H, width - yazici.margin_right + 4, height - YAZICI_FOOTER_H);

    /* Body Text */
    solar_os_gfx_set_font(gfx, YAZICI_FONT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    const int line_h = yazici.line_spacing >= 2U ? YAZICI_LINE_H_DOUBLE : YAZICI_LINE_H_SINGLE;
    const int visible = yazici_visible_line_count(gfx);

    for (int row = 0; row < visible; row++) {
        const size_t idx = yazici.scroll_top_line + (size_t)row;
        if (idx >= yazici.line_count) break;
        const yazici_line_t *line = &yazici.lines[idx];
        const int top = YAZICI_HEADER_H + row * line_h;
        const int baseline = top + YAZICI_BASELINE_OFS;

        if (line->start > 0U && (unsigned char)yazici.buffer[line->start - 1U] == '\f') {
            solar_os_gfx_line(gfx, yazici.margin_left, top + 2, width - yazici.margin_right, top + 2);
        }

        yazici_draw_line_text(gfx, yazici.margin_left, baseline, yazici.buffer + line->start, line->end - line->start);

        if (idx == yazici.cursor_line && yazici.blink_visible && !yazici.prompt_active) {
            const int cx = yazici.margin_left + yazici_x_for_offset(gfx, line, yazici.cursor);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, cx, top + 2, 2, line_h - 4);
        }
    }

    /* Footer */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, height - YAZICI_FOOTER_H, width, YAZICI_FOOTER_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    char footer[128];
    if (yazici.status_until_ms > yazici.elapsed_ms && yazici.status_message[0] != '\0') {
        snprintf(footer, sizeof(footer), "%s", yazici.status_message);
    } else {
        snprintf(footer, sizeof(footer), "[Ctrl+S] Save | [F2/Ctrl+N] New | [Ctrl+P] Spacing | [Ctrl+T] Sound | [ESC] Exit");
    }
    solar_os_gfx_text(gfx, 8, height - 6, footer);

    solar_os_gfx_present(gfx);
    yazici.render_pending = false;
}

static void yazici_move_vertical(int delta_lines)
{
    if (yazici.line_count == 0U) return;
    const yazici_line_t *cur = &yazici.lines[yazici.cursor_line];
    const size_t col = yazici.cursor >= cur->start ? yazici.cursor - cur->start : 0U;

    long target = (long)yazici.cursor_line + delta_lines;
    if (target < 0) target = 0;
    if (target >= (long)yazici.line_count) target = (long)yazici.line_count - 1;
    const yazici_line_t *dest = &yazici.lines[(size_t)target];
    size_t dest_len = dest->end - dest->start;
    size_t new_col = col < dest_len ? col : dest_len;
    yazici.cursor = dest->start + new_col;
}

static void yazici_handle_char(solar_os_context_t *ctx, char ch)
{
    const unsigned char uch = (unsigned char)ch;
    yazici.last_input_ms = yazici.elapsed_ms;
    yazici.blink_visible = true;
    yazici.blink_accum_ms = 0U;

    if (uch == SOLAR_OS_KEY_ESCAPE) {
        if (yazici.dirty) {
            (void)yazici_save_now();
        }
        yazici_save_settings();
        solar_os_context_request_exit(ctx);
        return;
    }

    if (uch == SOLAR_OS_KEY_LEFT) {
        yazici.cursor = yazici_utf8_prev(yazici.buffer, yazici.length, yazici.cursor);
        yazici.render_pending = true;
        return;
    }
    if (uch == SOLAR_OS_KEY_RIGHT) {
        yazici.cursor = yazici_utf8_next(yazici.buffer, yazici.length, yazici.cursor);
        yazici.render_pending = true;
        return;
    }
    if (uch == SOLAR_OS_KEY_UP) {
        yazici_move_vertical(-1);
        yazici.render_pending = true;
        return;
    }
    if (uch == SOLAR_OS_KEY_DOWN) {
        yazici_move_vertical(1);
        yazici.render_pending = true;
        return;
    }
    if (uch == SOLAR_OS_KEY_HOME) {
        if (yazici.cursor_line < yazici.line_count) {
            yazici.cursor = yazici.lines[yazici.cursor_line].start;
        }
        yazici.render_pending = true;
        return;
    }
    if (uch == SOLAR_OS_KEY_END) {
        if (yazici.cursor_line < yazici.line_count) {
            yazici.cursor = yazici.lines[yazici.cursor_line].end;
        }
        yazici.render_pending = true;
        return;
    }
    if (uch == SOLAR_OS_KEY_PAGE_UP) {
        yazici_move_vertical(-8);
        yazici.render_pending = true;
        return;
    }
    if (uch == SOLAR_OS_KEY_PAGE_DOWN) {
        yazici_move_vertical(8);
        yazici.render_pending = true;
        return;
    }
    if (uch == SOLAR_OS_KEY_DELETE) {
        yazici_delete_forward();
        yazici.render_pending = true;
        return;
    }

    /* Save: Ctrl+S (0x13) */
    if (uch == 0x13U) {
        (void)yazici_save_now();
        return;
    }

    /* New Document: Ctrl+N (0x0e) or F2 */
    if (uch == 0x0eU || uch == SOLAR_OS_KEY_F2) {
        if (yazici.dirty) {
            (void)yazici_save_now();
        }
        yazici.length = 0U;
        yazici.cursor = 0U;
        yazici.dirty = false;
        yazici.layout_pending = true;
        yazici.render_pending = true;
        yazici_set_status("New blank document ready");
        return;
    }

    /* Toggle Line Spacing: Ctrl+P (0x10) */
    if (uch == 0x10U || uch == SOLAR_OS_KEY_F3) {
        yazici.line_spacing = (yazici.line_spacing == 1U) ? 2U : 1U;
        yazici.layout_pending = true;
        yazici.render_pending = true;
        yazici_set_status(yazici.line_spacing == 2U ? "Double Line Spacing" : "Single Line Spacing");
        return;
    }

    /* Toggle Mechanical Sound: Ctrl+T (0x14) */
    if (uch == 0x14U || uch == SOLAR_OS_KEY_F4) {
        yazici.sound_enabled = !yazici.sound_enabled;
        yazici_set_status(yazici.sound_enabled ? "Mechanical Typewriter Sounds ON" : "Sounds Muted");
        return;
    }

    if (ch == '\n' || ch == '\r') {
        yazici_insert_text("\n", 1U);
        yazici_play_tone(YAZICI_TONE_RETURN_HZ, YAZICI_TONE_RETURN_MS);
        return;
    }
    if (ch == '\t') {
        yazici_insert_text("    ", 4U);
        yazici_play_tone(YAZICI_TONE_CLICK_HZ, YAZICI_TONE_CLICK_MS);
        return;
    }
    if (uch == 0x7fU || uch == 0x08U) {
        if (yazici.classic_mode) {
            yazici_backspace_classic();
        } else {
            yazici_backspace_normal();
        }
        return;
    }
    if (uch >= 0x20U) {
        yazici_insert_text(&ch, 1U);
        yazici_play_tone(YAZICI_TONE_CLICK_HZ, YAZICI_TONE_CLICK_MS);
        yazici.bell_check_pending = true;
        return;
    }
}

static bool yazici_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    switch (event->type) {
    case SOLAR_OS_EVENT_CHAR:
        yazici_handle_char(ctx, event->data.ch);
        break;
    case SOLAR_OS_EVENT_TICK:
        yazici.elapsed_ms += YAZICI_TICK_MS;
        yazici.blink_accum_ms += YAZICI_TICK_MS;
        if (yazici.blink_accum_ms >= YAZICI_BLINK_MS) {
            yazici.blink_accum_ms = 0U;
            yazici.blink_visible = !yazici.blink_visible;
            yazici.render_pending = true;
        }
        if (yazici.dirty && yazici.has_path &&
            (yazici.elapsed_ms - yazici.last_autosave_ms) >= yazici.autosave_interval_ms) {
            (void)yazici_save_now();
        }
        if (yazici.status_until_ms == yazici.elapsed_ms) {
            yazici.render_pending = true;
        }
        if (yazici.layout_pending || yazici.render_pending) {
            yazici_render(ctx);
        }
        break;
    case SOLAR_OS_EVENT_RESUME:
        yazici.render_pending = true;
        yazici_render(ctx);
        break;
    case SOLAR_OS_EVENT_CLICK: {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx != NULL && yazici.line_count > 0U && !yazici.prompt_active) {
            const int line_h = yazici.line_spacing >= 2U ? YAZICI_LINE_H_DOUBLE : YAZICI_LINE_H_SINGLE;
            const int row = (event->data.click.y - YAZICI_HEADER_H) / line_h;
            if (row >= 0) {
                size_t idx = yazici.scroll_top_line + (size_t)row;
                if (idx >= yazici.line_count) idx = yazici.line_count - 1U;
                const yazici_line_t *line = &yazici.lines[idx];
                yazici.cursor_line = idx;
                yazici.cursor = yazici_offset_for_x(gfx, line, event->data.click.x - yazici.margin_left);
                yazici.blink_visible = true;
                yazici.blink_accum_ms = 0U;
                yazici.render_pending = true;
                yazici_render(ctx);
            }
        }
        break;
    }
    case SOLAR_OS_EVENT_SCROLL: {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx != NULL && yazici.line_count > 0U) {
            const int visible = yazici_visible_line_count(gfx);
            const size_t max_top = (size_t)visible < yazici.line_count ? yazici.line_count - (size_t)visible : 0U;
            /* Sign is device-dependent -- flip here if it feels backwards. */
            if (event->data.scroll.delta < 0) {
                if (yazici.scroll_top_line < max_top) {
                    yazici.scroll_top_line += 3U;
                    if (yazici.scroll_top_line > max_top) yazici.scroll_top_line = max_top;
                }
            } else {
                yazici.scroll_top_line = yazici.scroll_top_line > 3U ? yazici.scroll_top_line - 3U : 0U;
            }
            yazici.render_pending = true;
            yazici_render(ctx);
        }
        break;
    }
    default:
        break;
    }
    return true;
}

static esp_err_t yazici_start(solar_os_context_t *ctx)
{
    memset(&yazici, 0, sizeof(yazici));
    yazici_load_settings();
    yazici.buffer = NULL;
    yazici.capacity = 0U;
    yazici.length = 0U;
    yazici.cursor = 0U;
    yazici.blink_visible = true;
    yazici.line_count = 0U;
    yazici.layout_pending = true;
    yazici.render_pending = true;

    if (!yazici_ensure_capacity(4096U)) {
        return ESP_ERR_NO_MEM;
    }

    if (solar_os_context_argc(ctx) > 1) {
        const char *arg = solar_os_context_argv(ctx, 1);
        char resolved[SOLAR_OS_STORAGE_PATH_MAX];
        if (solar_os_storage_resolve_path(arg, resolved, sizeof(resolved)) == ESP_OK) {
            yazici_load_file(resolved);
            strncpy(yazici.path, resolved, sizeof(yazici.path) - 1U);
            yazici.path[sizeof(yazici.path) - 1U] = '\0';
            yazici.has_path = true;
        }
    } else {
        const char *mount = solar_os_storage_sd_is_mounted() ?
            solar_os_storage_sd_mount_point() :
            (solar_os_storage_flash_is_mounted() ? solar_os_storage_flash_mount_point() : "/sdcard");
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/doc", mount);
        ensure_dir_exists(dir);
        snprintf(yazici.path, sizeof(yazici.path), "%s/typewriter.txt", dir);
        yazici.has_path = true;
    }

    solar_os_context_set_graphics_active(ctx, true);
    yazici_render(ctx);
    return ESP_OK;
}

static void yazici_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (yazici.dirty && yazici.has_path) {
        (void)yazici_save_now();
    }
    yazici_save_settings();
    if (yazici.buffer != NULL) {
        solar_os_memory_free(yazici.buffer);
        yazici.buffer = NULL;
    }
}

static void yazici_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    snprintf(buffer, buffer_len, "Typewriter: %s", yazici.has_path ? yazici.path : "untitled");
}

const solar_os_app_t solar_os_yazici_app = {
    .name = "yazici",
    .summary = "vintage mechanical typewriter simulator",
    .flags = 0,
    .start = yazici_start,
    .stop = yazici_stop,
    .event = yazici_event,
    .title = yazici_title,
    .state_slot = &yazici_state_ptr,
    .state_size = sizeof(yazici_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = YAZICI_TICK_MS,
    .worker_stack_bytes = 0,
};
