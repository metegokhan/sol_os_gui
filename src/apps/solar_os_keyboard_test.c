#include "solar_os_keyboard_test.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "solar_os_gfx.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_storage.h"
#include "solar_os_appbar.h"

#define KEYTEST_HISTORY_CAPACITY 8U

typedef struct {
    uint32_t seq;
    uint16_t usage;
    uint16_t physical_key;
    uint8_t logical_key;
    uint8_t modifiers;
    solar_os_input_key_action_t action;
    char name[24];
} keytest_history_item_t;

typedef struct {
    bool has_event;
    solar_os_input_key_event_t current_event;
    uint32_t total_events;
    solar_os_input_keyboard_layout_t active_layout;
    keytest_history_item_t history[KEYTEST_HISTORY_CAPACITY];
    size_t history_count;
} keytest_state_t;

static keytest_state_t s_keytest;

static const char *keytest_action_name(solar_os_input_key_action_t action)
{
    switch (action) {
    case SOLAR_OS_INPUT_KEY_PRESS:   return "PRESS";
    case SOLAR_OS_INPUT_KEY_RELEASE: return "RELEASE";
    case SOLAR_OS_INPUT_KEY_REPEAT:  return "REPEAT";
    default:                         return "UNKNOWN";
    }
}

static void keytest_get_key_label(const solar_os_input_key_event_t *ev, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) return;
    buf[0] = '\0';

    if (ev == NULL) {
        strlcpy(buf, "NONE", buf_len);
        return;
    }

    if (ev->key == SOLAR_OS_KEY_AUDIO_VOLUME_UP || ev->usage == 0x00E9 || ev->usage == 0x80 || ev->usage == 0x0001) {
        strlcpy(buf, "VOL_UP", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_AUDIO_VOLUME_DOWN || ev->usage == 0x00EA || ev->usage == 0x81 || ev->usage == 0x8000) {
        strlcpy(buf, "VOL_DOWN", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE || ev->usage == 0x00E2 || ev->usage == 0x7F || ev->usage == 0x4000) {
        strlcpy(buf, "MUTE", buf_len);
        return;
    }
    if (ev->usage == 0x00CD) {
        strlcpy(buf, "PLAY/PAUSE", buf_len);
        return;
    }
    if (ev->usage == 0x00B5) {
        strlcpy(buf, "NEXT_TRACK", buf_len);
        return;
    }
    if (ev->usage == 0x00B6) {
        strlcpy(buf, "PREV_TRACK", buf_len);
        return;
    }
    if (ev->usage == 0x0221) {
        strlcpy(buf, "SEARCH", buf_len);
        return;
    }
    if (ev->usage == 0x0224) {
        strlcpy(buf, "AC_BACK", buf_len);
        return;
    }
    if (ev->usage == 0x0225) {
        strlcpy(buf, "AC_FORWARD", buf_len);
        return;
    }
    if (ev->usage == 0x0223) {
        strlcpy(buf, "AC_HOME", buf_len);
        return;
    }
    if (ev->usage == 0x0192) {
        strlcpy(buf, "CALCULATOR", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_PRINT_SCREEN || ev->usage == 0x46) {
        strlcpy(buf, "PRTSC", buf_len);
        return;
    }
    if (ev->key == '\n' || ev->usage == 0x28) {
        strlcpy(buf, "ENTER", buf_len);
        return;
    }
    if (ev->key == ' ' || ev->usage == 0x2C) {
        strlcpy(buf, "SPACE", buf_len);
        return;
    }
    if (ev->key == '\b' || ev->usage == 0x2A) {
        strlcpy(buf, "BACKSPACE", buf_len);
        return;
    }
    if (ev->key == '\t' || ev->usage == 0x2B) {
        strlcpy(buf, "TAB", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_ESCAPE || ev->usage == 0x29) {
        strlcpy(buf, "ESCAPE", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_UP || ev->usage == 0x52) {
        strlcpy(buf, "UP", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_DOWN || ev->usage == 0x51) {
        strlcpy(buf, "DOWN", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_LEFT || ev->usage == 0x50) {
        strlcpy(buf, "LEFT", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_RIGHT || ev->usage == 0x4F) {
        strlcpy(buf, "RIGHT", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_DELETE || ev->usage == 0x4C) {
        strlcpy(buf, "DELETE", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_HOME || ev->usage == 0x4A) {
        strlcpy(buf, "HOME", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_END || ev->usage == 0x4D) {
        strlcpy(buf, "END", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_PAGE_UP || ev->usage == 0x4B) {
        strlcpy(buf, "PAGE_UP", buf_len);
        return;
    }
    if (ev->key == SOLAR_OS_KEY_PAGE_DOWN || ev->usage == 0x4E) {
        strlcpy(buf, "PAGE_DOWN", buf_len);
        return;
    }
    if (ev->key >= SOLAR_OS_KEY_F1 && ev->key <= SOLAR_OS_KEY_F12) {
        snprintf(buf, buf_len, "F%u", (unsigned)(ev->key - SOLAR_OS_KEY_F1 + 1));
        return;
    }

    const char *kn = solar_os_key_name(ev->key);
    if (kn != NULL && kn[0] != '\0') {
        strlcpy(buf, kn, buf_len);
        return;
    }

    if (ev->key >= 32 && ev->key <= 126) {
        snprintf(buf, buf_len, "'%c'", (char)ev->key);
        return;
    }

    snprintf(buf, buf_len, "KEY_0x%02X", ev->key);
}

static void keytest_get_layout_preview(uint16_t usage, uint8_t modifiers,
                                       solar_os_input_keyboard_layout_t layout,
                                       char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return;
    out[0] = '\0';

    solar_os_input_keyboard_layout_t old = solar_os_input_keyboard_layout();
    (void)solar_os_input_set_keyboard_layout(layout);
    uint8_t ch = solar_os_input_translate_hid_usage(usage, modifiers, false);
    (void)solar_os_input_set_keyboard_layout(old);

    if (ch >= 32 && ch <= 126) {
        snprintf(out, out_len, "%c", (char)ch);
    } else if (ch == '\n') {
        strlcpy(out, "\\n", out_len);
    } else if (ch == '\t') {
        strlcpy(out, "\\t", out_len);
    } else if (ch != 0) {
        snprintf(out, out_len, "0x%02X", ch);
    } else {
        strlcpy(out, "-", out_len);
    }
}

/* Short code shown on the footer Layout chip. */
static const char *keytest_layout_code(solar_os_input_keyboard_layout_t layout)
{
    switch (layout) {
    case SOLAR_OS_INPUT_KEYBOARD_LAYOUT_TR: return "TR";
    case SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE: return "DE";
    default:                                return "EN";
    }
}

static void keytest_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Shared header bar. */
    solar_os_appbar_header_t header = {0};
    header.title = "Keyboard Tester";
    header.show_back = true;
    solar_os_appbar_draw_header(gfx, &header);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    /* 2. Device & Connection Status Strip */
    char status_str[80] = "BLE: Disconnected";
    solar_os_ble_keyboard_get_status(status_str, sizeof(status_str));

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char conn_line[128];
    snprintf(conn_line, sizeof(conn_line), "Status: %s", status_str);
    solar_os_gfx_text(gfx, 8, 36, conn_line);

    /* Modifier Badges */
    uint8_t mods = s_keytest.has_event ? s_keytest.current_event.modifiers : 0;
    struct { const char *name; uint8_t mask; int x; } mod_badges[] = {
        {"SHIFT", SOLAR_OS_INPUT_MOD_SHIFT, 230},
        {"CTRL",  SOLAR_OS_INPUT_MOD_CTRL,  272},
        {"ALT",   SOLAR_OS_INPUT_MOD_LEFT_ALT, 308},
        {"ALTGR", SOLAR_OS_INPUT_MOD_RIGHT_ALT, 340},
        {"GUI",   (SOLAR_OS_INPUT_MOD_LEFT_GUI | SOLAR_OS_INPUT_MOD_RIGHT_GUI), 376},
    };

    for (size_t i = 0; i < 5; i++) {
        bool active = (mods & mod_badges[i].mask) != 0;
        int bx = mod_badges[i].x;
        int bw = (int)solar_os_gfx_text_width(gfx, mod_badges[i].name) + 6;
        if (active) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, bx, 25, bw, 14);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_text(gfx, bx + 3, 36, mod_badges[i].name);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, bx, 25, bw, 14);
            solar_os_gfx_text(gfx, bx + 3, 36, mod_badges[i].name);
        }
    }

    solar_os_gfx_line(gfx, 0, 44, 400, 44);

    /* 3. Hero Visualizer Card (Left: 6, 48, 126, 102) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 6, 48, 126, 102);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 12, 60, "LAST KEY EVENT");

    char key_label[24] = "WAITING";
    if (s_keytest.has_event) {
        keytest_get_key_label(&s_keytest.current_event, key_label, sizeof(key_label));
    }

    bool is_press = s_keytest.has_event && s_keytest.current_event.action != SOLAR_OS_INPUT_KEY_RELEASE;
    if (is_press) {
        solar_os_gfx_fill_rect(gfx, 12, 66, 114, 52);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    } else {
        solar_os_gfx_rect(gfx, 12, 66, 114, 52);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    }

    solar_os_gfx_set_font(gfx, strlen(key_label) <= 4 ? SOLAR_OS_GFX_FONT_BOLD_20 : SOLAR_OS_GFX_FONT_BOLD_14);
    const size_t kw = solar_os_gfx_text_width(gfx, key_label);
    int kx = 12 + (114 - (int)kw) / 2;
    if (kx < 14) kx = 14;
    solar_os_gfx_text(gfx, kx, 98, key_label);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const char *act_str = s_keytest.has_event ? keytest_action_name(s_keytest.current_event.action) : "IDLE";
    char act_buf[32];
    snprintf(act_buf, sizeof(act_buf), "State: %s", act_str);
    solar_os_gfx_text(gfx, 12, 136, act_buf);

    /* 4. Raw Telemetry Breakdown Card (Right: 138, 48, 256, 102) */
    solar_os_gfx_rect(gfx, 138, 48, 256, 102);
    solar_os_gfx_text(gfx, 144, 60, "RAW HID & TELEMETRY DATA");

    if (s_keytest.has_event) {
        const solar_os_input_key_event_t *ev = &s_keytest.current_event;
        char l1[64], l2[64], l3[64], l4[64];

        snprintf(l1, sizeof(l1), "USB Usage Code : 0x%04X (%u)", ev->usage, ev->usage);
        snprintf(l2, sizeof(l2), "Physical Key   : 0x%04X | Src: %u", ev->physical_key, ev->source);
        snprintf(l3, sizeof(l3), "SolarOS KeyCode: 0x%02X (%u)", ev->key, ev->key);

        char tr_sym[8], us_sym[8], de_sym[8];
        keytest_get_layout_preview(ev->usage, ev->modifiers, SOLAR_OS_INPUT_KEYBOARD_LAYOUT_TR, tr_sym, sizeof(tr_sym));
        keytest_get_layout_preview(ev->usage, ev->modifiers, SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US, us_sym, sizeof(us_sym));
        keytest_get_layout_preview(ev->usage, ev->modifiers, SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, de_sym, sizeof(de_sym));
        snprintf(l4, sizeof(l4), "Lang Syms: TR:[%s] US:[%s] DE:[%s]", tr_sym, us_sym, de_sym);

        solar_os_gfx_text(gfx, 144, 76, l1);
        solar_os_gfx_text(gfx, 144, 92, l2);
        solar_os_gfx_text(gfx, 144, 108, l3);
        solar_os_gfx_text(gfx, 144, 124, l4);
    } else {
        solar_os_gfx_text(gfx, 144, 86, "Press any key on keyboard...");
        solar_os_gfx_text(gfx, 144, 106, "(Media keys, Fn, Modifiers supported)");
    }

    /* 5. In-Memory Live History Panel (bottom, leaves room for the footer). */
    solar_os_gfx_rect(gfx, 6, 154, 388, 120);
    solar_os_gfx_fill_rect(gfx, 6, 154, 388, 18);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 12, 167, "LIVE KEY LOG (recent events)");

    char total_buf[32];
    snprintf(total_buf, sizeof(total_buf), "Total: %lu", (unsigned long)s_keytest.total_events);
    solar_os_gfx_text(gfx, 320, 167, total_buf);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 10, 185, "#");
    solar_os_gfx_text(gfx, 36, 185, "Key Name");
    solar_os_gfx_text(gfx, 120, 185, "Usage");
    solar_os_gfx_text(gfx, 175, 185, "Phys");
    solar_os_gfx_text(gfx, 225, 185, "KeyCode");
    solar_os_gfx_text(gfx, 285, 185, "Mods");
    solar_os_gfx_text(gfx, 335, 185, "Action");
    solar_os_gfx_line(gfx, 6, 189, 394, 189);

    int row_y = 202;
    for (size_t i = 0; i < s_keytest.history_count && i < 5; i++) {
        const keytest_history_item_t *item = &s_keytest.history[i];
        char seq_str[12], u_str[12], p_str[12], k_str[12], m_str[16];
        snprintf(seq_str, sizeof(seq_str), "%02lu", (unsigned long)(item->seq % 100));
        snprintf(u_str, sizeof(u_str), "0x%04X", item->usage);
        snprintf(p_str, sizeof(p_str), "0x%04X", item->physical_key);
        snprintf(k_str, sizeof(k_str), "0x%02X", item->logical_key);

        char mod_txt[8] = "-";
        if (item->modifiers != 0) {
            snprintf(mod_txt, sizeof(mod_txt), "%s%s%s",
                     (item->modifiers & SOLAR_OS_INPUT_MOD_SHIFT) ? "S" : "",
                     (item->modifiers & SOLAR_OS_INPUT_MOD_CTRL) ? "C" : "",
                     (item->modifiers & SOLAR_OS_INPUT_MOD_ALT) ? "A" : "");
        }
        snprintf(m_str, sizeof(m_str), "%s", mod_txt);

        solar_os_gfx_text(gfx, 10, row_y, seq_str);
        solar_os_gfx_text(gfx, 36, row_y, item->name);
        solar_os_gfx_text(gfx, 120, row_y, u_str);
        solar_os_gfx_text(gfx, 175, row_y, p_str);
        solar_os_gfx_text(gfx, 225, row_y, k_str);
        solar_os_gfx_text(gfx, 285, row_y, m_str);
        solar_os_gfx_text(gfx, 335, row_y, keytest_action_name(item->action));

        row_y += 15;
    }

    /* Footer chips (Esc/Tab are universal, so only the non-obvious ones).
     * The Layout chip carries the active layout code so no separate top line
     * is needed. */
    solar_os_appbar_shortcut_t footer_items[2] = {
        { .key = 'l', .ctrl = false },
        { .key = 'c', .ctrl = false, .label = "Clear" },
    };
    snprintf(footer_items[0].label, sizeof(footer_items[0].label), "Layout:%s",
             keytest_layout_code(s_keytest.active_layout));
    const solar_os_appbar_shortcuts_t footer = { .items = footer_items, .count = 2 };
    solar_os_appbar_draw_footer(gfx, &footer);

    solar_os_gfx_present(gfx);
}

static void keytest_push_history(const solar_os_input_key_event_t *ev)
{
    if (ev == NULL) return;

    for (size_t i = KEYTEST_HISTORY_CAPACITY - 1; i > 0; i--) {
        s_keytest.history[i] = s_keytest.history[i - 1];
    }

    keytest_history_item_t *item = &s_keytest.history[0];
    s_keytest.total_events++;
    item->seq = s_keytest.total_events;
    item->usage = ev->usage;
    item->physical_key = ev->physical_key;
    item->logical_key = ev->key;
    item->modifiers = ev->modifiers;
    item->action = ev->action;
    keytest_get_key_label(ev, item->name, sizeof(item->name));

    if (s_keytest.history_count < KEYTEST_HISTORY_CAPACITY) {
        s_keytest.history_count++;
    }
}

static esp_err_t keytest_start(solar_os_context_t *ctx)
{
    memset(&s_keytest, 0, sizeof(s_keytest));
    s_keytest.active_layout = solar_os_input_keyboard_layout();
    solar_os_context_set_graphics_active(ctx, true);
    keytest_render(ctx);
    return ESP_OK;
}

static void keytest_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void keytest_suspend(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void keytest_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    keytest_render(ctx);
}

static void keytest_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer && buffer_len > 0) {
        strlcpy(buffer, "keytest", buffer_len);
    }
}

static bool keytest_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        keytest_resume(ctx);
        return true;
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

        solar_os_appbar_shortcut_t footer_items[2] = {
            { .key = 'l', .ctrl = false },
            { .key = 'c', .ctrl = false, .label = "Clear" },
        };
        snprintf(footer_items[0].label, sizeof(footer_items[0].label), "Layout:%s",
                 keytest_layout_code(s_keytest.active_layout));
        const solar_os_appbar_shortcuts_t footer = { .items = footer_items, .count = 2 };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &footer, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM) {
                if (fhit.index == 0) {
                    s_keytest.active_layout = (s_keytest.active_layout + 1) % 3;
                    (void)solar_os_input_set_keyboard_layout(s_keytest.active_layout);
                } else {
                    s_keytest.history_count = 0;
                    s_keytest.total_events = 0;
                    s_keytest.has_event = false;
                }
                keytest_render(ctx);
            }
            return true;
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_KEY) {
        const solar_os_input_key_event_t *ev = &event->data.key;

        if (ev->key == SOLAR_OS_KEY_APP_EXIT ||
            (ev->key == SOLAR_OS_KEY_ESCAPE && ev->action == SOLAR_OS_INPUT_KEY_PRESS)) {
            solar_os_context_request_exit(ctx);
            return true;
        }

        if (ev->action == SOLAR_OS_INPUT_KEY_PRESS) {
            if (ev->key == '\t' || ev->key == 'l' || ev->key == 'L') {
                if (ev->key == '\t' || (ev->modifiers & (SOLAR_OS_INPUT_MOD_CTRL | SOLAR_OS_INPUT_MOD_ALT)) != 0) {
                    s_keytest.active_layout = (s_keytest.active_layout + 1) % 3;
                    (void)solar_os_input_set_keyboard_layout(s_keytest.active_layout);
                }
            }
            if (ev->key == 'c' || ev->key == 'C') {
                if ((ev->modifiers & (SOLAR_OS_INPUT_MOD_CTRL | SOLAR_OS_INPUT_MOD_ALT)) != 0) {
                    s_keytest.history_count = 0;
                    s_keytest.total_events = 0;
                    s_keytest.has_event = false;
                    keytest_render(ctx);
                    return true;
                }
            }
        }

        s_keytest.current_event = *ev;
        s_keytest.has_event = true;
        keytest_push_history(ev);
        keytest_render(ctx);
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;
        if (ch == (char)SOLAR_OS_KEY_APP_EXIT || ch == (char)SOLAR_OS_KEY_ESCAPE) {
            solar_os_context_request_exit(ctx);
            return true;
        }
        if (ch == 'c' || ch == 'C') {
            s_keytest.history_count = 0;
            s_keytest.total_events = 0;
            s_keytest.has_event = false;
            keytest_render(ctx);
            return true;
        }
        if (ch == '\t' || ch == 'l' || ch == 'L') {
            s_keytest.active_layout = (s_keytest.active_layout + 1) % 3;
            (void)solar_os_input_set_keyboard_layout(s_keytest.active_layout);
            keytest_render(ctx);
            return true;
        }
    }

    return true;
}

const solar_os_app_t solar_os_keyboard_test_app = {
    .name = "keytest",
    .summary = "live keyboard tester and input inspector",
    .flags = SOLAR_OS_APP_FLAG_KEY_EVENTS,
    .start = keytest_start,
    .suspend = keytest_suspend,
    .resume = keytest_resume,
    .stop = keytest_stop,
    .event = keytest_event,
    .title = keytest_title,
};
