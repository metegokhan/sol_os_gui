/*
 * Tabela - ekrani dolduran, kayan yazi (LED tabela) uygulamasi
 * (SolarOS native uygulamasi)
 *
 * Bu dosya SolarOS kaynak agacinin DISINDA gelistirilmektedir
 * (bkz. ../../docs/solaros_native_app_notes.md ve ../../integration/README.md).
 *
 * ONEMLI TASARIM NOTU: SolarOS'un derleme-zamaninda uretilen fontlari
 * sabit boyutludur (en buyugu 20px, FONT_BOLD_20) ve calisma zamaninda
 * buyutulemez -- bu, "ekran yuksekligine uygun, en buyuk boyutta"
 * gereksinimini karsilamaya yetmez (400x300'luk bir ekranda 20px cok
 * kucuk kalir). Bu yuzden bu uygulama kendi basit 5x7 nokta-matrisi
 * fontunu kullanir ve onu calisma zamaninda ekran yuksekligine gore
 * (kucuk ust/alt boslukla) piksel-blok olarak olceklendirir -- klasik
 * bir LED tabela gibi. Font, harflerin dogrulugu kolay gorsel
 * denetlenebilsin diye satir-satir '#'/'.' dizeleri olarak tanimli.
 */

#include "solar_os_tabela.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_appbar.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"

#define TABELA_TICK_MS 40U
#define TABELA_TEXT_MAX 121U
#define TABELA_HEADER_H 22
#define TABELA_FOOTER_H 18
#define TABELA_SCREEN_MARGIN 10
#define TABELA_GLYPH_GAP_COLS 1
#define TABELA_FONT_COLS 5
#define TABELA_FONT_ROWS 7
#define TABELA_SPEED_MIN 30U
#define TABELA_SPEED_MAX 500U
#define TABELA_SPEED_DEFAULT 130U
#define TABELA_SPEED_STEP 15U

typedef struct {
    char ch;
    const char *rows[TABELA_FONT_ROWS];
} tabela_glyph_t;

/* 5 sutun x 7 satir nokta-matrisi font. '#' = yanik piksel, '.' = sonuk. */
static const tabela_glyph_t TABELA_FONT[] = {
    {' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".####", "#....", "#....", "#....", "#....", "#....", ".####"}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".####", "#....", "#....", "#.###", "#...#", "#...#", ".####"}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "##..#", "#.#.#", "#.#.#", "#..##", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"####.", "....#", "....#", ".###.", "....#", "....#", "####."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "#....", "####.", "....#", "....#", "####."}},
    {'6', {".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {',', {".....", ".....", ".....", ".....", ".....", "..#..", ".#..."}},
    {'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
    {'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {';', {".....", ".##..", ".##..", ".....", ".##..", ".#...", "....."}},
    {'\'', {".##..", ".##..", ".#...", ".....", ".....", ".....", "....."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {'/', {"....#", "...#.", "..#..", "..#..", ".#...", "#....", "....."}},
    {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
    {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
};
#define TABELA_FONT_COUNT (sizeof(TABELA_FONT) / sizeof(TABELA_FONT[0]))

typedef struct {
    char text[TABELA_TEXT_MAX];
    size_t text_len;

    bool composing;
    char compose_buffer[TABELA_TEXT_MAX];
    size_t compose_len;

    float scroll_x;
    int scale;
    int glyph_h_px;
    int char_w_px;
    int top_y;
    int text_pixel_width;
    int screen_width;
    bool needs_layout;

    uint32_t speed_px_per_sec;
    bool inverted;

    uint32_t elapsed_ms;
    bool render_pending;
    char status_message[64];
    uint32_t status_until_ms;
} tabela_state_t;

static void *tabela_state_ptr;
#define tabela (*(tabela_state_t *)tabela_state_ptr)

static void tabela_render(solar_os_context_t *ctx);

/* ---------------------------------------------------------------------
 * Font cizimi
 * ------------------------------------------------------------------- */

static const tabela_glyph_t *tabela_find_glyph(char c)
{
    const char up = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    for (size_t i = 0U; i < TABELA_FONT_COUNT; i++) {
        if (TABELA_FONT[i].ch == up) {
            return &TABELA_FONT[i];
        }
    }
    return NULL;
}

static void tabela_draw_glyph(solar_os_gfx_t *gfx, const tabela_glyph_t *glyph, int x, int y,
                              int scale)
{
    if (glyph == NULL) {
        return;
    }
    for (int row = 0; row < TABELA_FONT_ROWS; row++) {
        const char *line = glyph->rows[row];
        for (int col = 0; col < TABELA_FONT_COLS; col++) {
            if (line[col] == '#') {
                solar_os_gfx_fill_rect(gfx, x + col * scale, y + row * scale, scale, scale);
            }
        }
    }
}

/* ---------------------------------------------------------------------
 * Duzen / kaydirma
 * ------------------------------------------------------------------- */

static void tabela_compute_layout(solar_os_gfx_t *gfx)
{
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    int available = height - TABELA_SCREEN_MARGIN * 2;
    if (available < TABELA_FONT_ROWS) {
        available = TABELA_FONT_ROWS;
    }
    tabela.scale = available / TABELA_FONT_ROWS;
    if (tabela.scale < 1) {
        tabela.scale = 1;
    }
    tabela.glyph_h_px = TABELA_FONT_ROWS * tabela.scale;
    tabela.top_y = (height - tabela.glyph_h_px) / 2;
    tabela.char_w_px = (TABELA_FONT_COLS + TABELA_GLYPH_GAP_COLS) * tabela.scale;
    tabela.text_pixel_width = (int)tabela.text_len * tabela.char_w_px;
    tabela.screen_width = width;
    tabela.scroll_x = (float)width;
    tabela.needs_layout = false;
}

static void tabela_advance_scroll(void)
{
    if (tabela.text_len == 0U || tabela.needs_layout) {
        return;
    }
    const float delta = (float)tabela.speed_px_per_sec * (float)TABELA_TICK_MS / 1000.0f;
    tabela.scroll_x -= delta;
    if (tabela.scroll_x < -(float)tabela.text_pixel_width) {
        tabela.scroll_x = (float)tabela.screen_width;
    }
}

static void tabela_start_scroll(const char *text)
{
    strncpy(tabela.text, text, sizeof(tabela.text) - 1U);
    tabela.text[sizeof(tabela.text) - 1U] = '\0';
    tabela.text_len = strlen(tabela.text);
    tabela.composing = false;
    tabela.needs_layout = true;
    tabela.render_pending = true;
}

/* ---------------------------------------------------------------------
 * Durum satiri
 * ------------------------------------------------------------------- */

static void tabela_set_status(const char *message)
{
    strncpy(tabela.status_message, message, sizeof(tabela.status_message) - 1U);
    tabela.status_message[sizeof(tabela.status_message) - 1U] = '\0';
    tabela.status_until_ms = tabela.elapsed_ms + 2000U;
    tabela.render_pending = true;
}

/* ---------------------------------------------------------------------
 * Cizim
 * ------------------------------------------------------------------- */

static void tabela_draw_compose(solar_os_gfx_t *gfx, int width, int height)
{
    (void)height;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 10, TABELA_HEADER_H + 20, "Type billboard message, press [Enter] to start:");

    solar_os_gfx_rect(gfx, 8, TABELA_HEADER_H + 30, width - 16, 28);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_16);
    char shown[TABELA_TEXT_MAX + 1U];
    snprintf(shown, sizeof(shown), "%s_", tabela.compose_buffer);
    solar_os_gfx_text(gfx, 12, TABELA_HEADER_H + 50, shown);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 10, TABELA_HEADER_H + 90,
                      "Supported chars: A-Z, 0-9, space, . , ! ? : ; ' - + / ( )");
}

static void tabela_build_header(solar_os_appbar_header_t *out)
{
    memset(out, 0, sizeof(*out));
    out->title = "Marquee";
    out->show_back = true;
}

static void tabela_draw_header(solar_os_gfx_t *gfx)
{
    solar_os_appbar_header_t header;
    tabela_build_header(&header);
    solar_os_appbar_draw_header(gfx, &header);
}

/* Builds the current footer's shortcut chips into a caller-owned buffer,
 * returning the count. Same set used by both drawing and click hit-testing
 * so they can never disagree about what's on screen. */
static size_t tabela_build_footer_shortcuts(solar_os_appbar_shortcut_t *items, size_t max_items)
{
    size_t n = 0;
    if (n < max_items) { items[n].key = '\n'; items[n].ctrl = false; snprintf(items[n].label, sizeof(items[n].label), "Start"); n++; }
    return n;
}

static void tabela_draw_footer(solar_os_gfx_t *gfx, int width, int height)
{
    (void)width;
    (void)height;
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = tabela_build_footer_shortcuts(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);
}

static void tabela_draw_scroll(solar_os_gfx_t *gfx, int width)
{
    solar_os_gfx_clear(gfx, tabela.inverted ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(gfx,
                           tabela.inverted ? SOLAR_OS_GFX_COLOR_WHITE : SOLAR_OS_GFX_COLOR_BLACK);

    int x = (int)tabela.scroll_x;
    for (size_t i = 0U; i < tabela.text_len; i++) {
        if (x > width) {
            break;
        }
        if (x + tabela.char_w_px > 0) {
            tabela_draw_glyph(gfx, tabela_find_glyph(tabela.text[i]), x, tabela.top_y,
                              tabela.scale);
        }
        x += tabela.char_w_px;
    }

    if (tabela.status_until_ms > tabela.elapsed_ms && tabela.status_message[0] != '\0') {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 4, (int)solar_os_gfx_height(gfx) - 4, tabela.status_message);
    }
}

static void tabela_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);

    if (tabela.composing) {
        solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        tabela_draw_header(gfx);
        tabela_draw_compose(gfx, width, height);
        tabela_draw_footer(gfx, width, height);
    } else {
        if (tabela.needs_layout) {
            tabela_compute_layout(gfx);
        }
        tabela_draw_scroll(gfx, width);
    }

    solar_os_gfx_present(gfx);
    tabela.render_pending = false;
}

static void tabela_handle_compose_char(char ch)
{
    const unsigned char uch = (unsigned char)ch;
    if (uch == 0x7fU || uch == 0x08U) {
        if (tabela.compose_len > 0U) {
            tabela.compose_len--;
            tabela.compose_buffer[tabela.compose_len] = '\0';
        }
        tabela.render_pending = true;
        return;
    }
    if (ch == '\n' || ch == '\r') {
        if (tabela.compose_len == 0U) {
            tabela_set_status("Please enter some text first");
            return;
        }
        tabela_start_scroll(tabela.compose_buffer);
        return;
    }
    if (uch >= 0x20U && uch < 0x80U && tabela.compose_len + 1U < sizeof(tabela.compose_buffer)) {
        tabela.compose_buffer[tabela.compose_len++] = ch;
        tabela.compose_buffer[tabela.compose_len] = '\0';
        tabela.render_pending = true;
    }
}

static void tabela_handle_char(solar_os_context_t *ctx, char ch)
{
    const unsigned char uch = (unsigned char)ch;

    if (uch == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_request_exit(ctx);
        return;
    }

    if (tabela.composing) {
        tabela_handle_compose_char(ch);
        return;
    }

    /* scrolling marquee mode */
    if (ch == 'e' || ch == 'E' || ch == '\n' || ch == '\r') {
        strncpy(tabela.compose_buffer, tabela.text, sizeof(tabela.compose_buffer) - 1U);
        tabela.compose_buffer[sizeof(tabela.compose_buffer) - 1U] = '\0';
        tabela.compose_len = strlen(tabela.compose_buffer);
        tabela.composing = true;
        tabela.render_pending = true;
        return;
    }
    if (ch == '+' || ch == '=') {
        if (tabela.speed_px_per_sec + TABELA_SPEED_STEP <= TABELA_SPEED_MAX) {
            tabela.speed_px_per_sec += TABELA_SPEED_STEP;
        }
        tabela_set_status("Speed Increased");
        return;
    }
    if (ch == '-' || ch == '_') {
        if (tabela.speed_px_per_sec > TABELA_SPEED_MIN + TABELA_SPEED_STEP) {
            tabela.speed_px_per_sec -= TABELA_SPEED_STEP;
        }
        tabela_set_status("Speed Decreased");
        return;
    }
    if (ch == 'i' || ch == 'I') {
        tabela.inverted = !tabela.inverted;
        tabela.render_pending = true;
        return;
    }
}

static bool tabela_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    switch (event->type) {
    case SOLAR_OS_EVENT_CHAR:
        tabela_handle_char(ctx, event->data.ch);
        break;
    case SOLAR_OS_EVENT_TICK:
        tabela.elapsed_ms += TABELA_TICK_MS;
        if (!tabela.composing) {
            tabela_advance_scroll();
            tabela.render_pending = true;
        }
        if (tabela.status_until_ms != 0U && tabela.status_until_ms <= tabela.elapsed_ms &&
            tabela.status_until_ms + TABELA_TICK_MS > tabela.elapsed_ms) {
            tabela.render_pending = true;
        }
        if (tabela.render_pending) {
            tabela_render(ctx);
        }
        break;
    case SOLAR_OS_EVENT_RESUME:
        tabela.needs_layout = true;
        tabela.render_pending = true;
        tabela_render(ctx);
        break;

    case SOLAR_OS_EVENT_CLICK: {
        if (!tabela.composing) break; /* fullscreen marquee has no chrome to hit-test */
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) break;

        solar_os_appbar_header_t header;
        tabela_build_header(&header);

        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, event->data.click.x, event->data.click.y, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                solar_os_context_request_exit(ctx);
            }
            break;
        }

        solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
        const size_t count = tabela_build_footer_shortcuts(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };

        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, event->data.click.x, event->data.click.y, &fhit) &&
            fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM) {
            tabela_handle_char(ctx, items[fhit.index].key);
            if (tabela.render_pending) tabela_render(ctx);
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

static esp_err_t tabela_start(solar_os_context_t *ctx)
{
    memset(&tabela, 0, sizeof(tabela));
    tabela.speed_px_per_sec = TABELA_SPEED_DEFAULT;
    tabela.render_pending = true;

    solar_os_context_set_graphics_active(ctx, true);

    if (solar_os_context_argc(ctx) > 1) {
        const char *arg = solar_os_context_argv(ctx, 1);
        tabela_start_scroll(arg);
    } else {
        tabela.composing = true;
        strncpy(tabela.compose_buffer, "SOLAROS", sizeof(tabela.compose_buffer) - 1U);
        tabela.compose_len = strlen(tabela.compose_buffer);
    }

    tabela_render(ctx);
    return ESP_OK;
}

static void tabela_suspend(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void tabela_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    tabela.needs_layout = true;
    tabela.render_pending = true;
    tabela_render(ctx);
}

static void tabela_stop(solar_os_context_t *ctx)
{
    (void)ctx;
}

static void tabela_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    snprintf(buffer, buffer_len, "Tabela: %s", tabela.text[0] != '\0' ? tabela.text : "(bos)");
}

const solar_os_app_t solar_os_tabela_app = {
    .name = "tabela",
    .summary = "ekrani dolduran kayan yazi (LED tabela)",
    .flags = 0,
    .start = tabela_start,
    .suspend = tabela_suspend,
    .resume = tabela_resume,
    .stop = tabela_stop,
    .event = tabela_event,
    .title = tabela_title,
    .state_slot = &tabela_state_ptr,
    .state_size = sizeof(tabela_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = TABELA_TICK_MS,
    .tick_deadline_ms = TABELA_TICK_MS,
};
