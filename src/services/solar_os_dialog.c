#include "solar_os_dialog.h"

#include <string.h>
#include <stdio.h>
#include "solar_os_keys.h"

typedef enum {
    SOLAR_OS_DIALOG_TYPE_EXIT = 0,
    SOLAR_OS_DIALOG_TYPE_MESSAGE,
} solar_os_dialog_type_t;

typedef struct {
    int x, y, w, h;
} dialog_button_rect_t;

typedef struct {
    bool active;
    solar_os_dialog_type_t type;
    char title[32];
    char message[128];
    char app_name[32];
    bool resumable;
    int selected_button;
} solar_os_dialog_state_t;

static solar_os_dialog_state_t dialog_state;

void solar_os_dialog_show_exit(const char *app_name, bool resumable)
{
    dialog_state.active = true;
    dialog_state.type = SOLAR_OS_DIALOG_TYPE_EXIT;
    if (app_name != NULL && app_name[0] != '\0') {
        strlcpy(dialog_state.app_name, app_name, sizeof(dialog_state.app_name));
    } else {
        strlcpy(dialog_state.app_name, "Application", sizeof(dialog_state.app_name));
    }
    dialog_state.resumable = resumable;
    /* Default focused button: 0 (Yes / Evet) as requested by user */
    dialog_state.selected_button = 0;
}

void solar_os_dialog_show_message(const char *title, const char *message)
{
    dialog_state.active = true;
    dialog_state.type = SOLAR_OS_DIALOG_TYPE_MESSAGE;
    if (title != NULL && title[0] != '\0') {
        strlcpy(dialog_state.title, title, sizeof(dialog_state.title));
    } else {
        strlcpy(dialog_state.title, "System Notice", sizeof(dialog_state.title));
    }
    if (message != NULL && message[0] != '\0') {
        strlcpy(dialog_state.message, message, sizeof(dialog_state.message));
    } else {
        strlcpy(dialog_state.message, "An error occurred.", sizeof(dialog_state.message));
    }
    dialog_state.resumable = false;
    dialog_state.selected_button = 0;
}

bool solar_os_dialog_is_active(void)
{
    return dialog_state.active;
}

void solar_os_dialog_close(void)
{
    dialog_state.active = false;
    dialog_state.app_name[0] = '\0';
    dialog_state.title[0] = '\0';
    dialog_state.message[0] = '\0';
    dialog_state.resumable = false;
}

static void dialog_get_rects(int screen_w, int screen_h,
                             int *dx, int *dy, int *dw, int *dh,
                             dialog_button_rect_t buttons[3], int *btn_count)
{
    *dw = 340;
    *dh = 160;
    if (*dw > screen_w - 20) *dw = screen_w - 20;
    if (*dh > screen_h - 20) *dh = screen_h - 20;
    *dx = (screen_w - *dw) / 2;
    *dy = (screen_h - *dh) / 2;

    const int btn_y = *dy + *dh - 46;
    const int btn_h = 32;

    if (dialog_state.type == SOLAR_OS_DIALOG_TYPE_MESSAGE) {
        *btn_count = 1;
        const int btn_w = 120;
        buttons[0] = (dialog_button_rect_t){ *dx + (*dw - btn_w) / 2, btn_y, btn_w, btn_h };
    } else if (dialog_state.resumable) {
        *btn_count = 3;
        const int gap = 8;
        const int total_btn_w = *dw - 28;
        const int btn_w = (total_btn_w - 2 * gap) / 3;

        buttons[0] = (dialog_button_rect_t){ *dx + 14, btn_y, btn_w, btn_h };
        buttons[1] = (dialog_button_rect_t){ *dx + 14 + btn_w + gap, btn_y, btn_w, btn_h };
        buttons[2] = (dialog_button_rect_t){ *dx + 14 + 2 * (btn_w + gap), btn_y, btn_w, btn_h };
    } else {
        *btn_count = 2;
        const int gap = 14;
        const int total_btn_w = *dw - 40;
        const int btn_w = (total_btn_w - gap) / 2;

        buttons[0] = (dialog_button_rect_t){ *dx + 20, btn_y, btn_w, btn_h };
        buttons[1] = (dialog_button_rect_t){ *dx + 20 + btn_w + gap, btn_y, btn_w, btn_h };
    }
}

void solar_os_dialog_draw(solar_os_gfx_t *gfx)
{
    if (gfx == NULL || !dialog_state.active) {
        return;
    }

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    int dx, dy, dw, dh;
    dialog_button_rect_t buttons[3];
    int btn_count = 0;
    dialog_get_rects(screen_w, screen_h, &dx, &dy, &dw, &dh, buttons, &btn_count);

    /* Dialog Background & Double Border */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, dx, dy, dw, dh);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, dx, dy, dw, dh);
    solar_os_gfx_rect(gfx, dx + 1, dy + 1, dw - 2, dh - 2);

    /* Title Bar */
    solar_os_gfx_line(gfx, dx, dy + 24, dx + dw, dy + 24);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, dx + 12, dy + 18,
                      dialog_state.type == SOLAR_OS_DIALOG_TYPE_MESSAGE ? dialog_state.title : "Exit Application");

    /* Body Text */
    if (dialog_state.type == SOLAR_OS_DIALOG_TYPE_MESSAGE) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, dx + 16, dy + 54, dialog_state.message);
    } else {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        char msg_line1[64];
        snprintf(msg_line1, sizeof(msg_line1), "Exit \"%s\"?", dialog_state.app_name);
        solar_os_gfx_text(gfx, dx + 16, dy + 52, msg_line1);
        solar_os_gfx_text(gfx, dx + 16, dy + 70, "Are you sure you want to close this app?");

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        if (dialog_state.resumable) {
            solar_os_gfx_text(gfx, dx + 16, dy + 92, "Minimize keeps it running in the background.");
        } else {
            solar_os_gfx_text(gfx, dx + 16, dy + 92, "Unsaved temporary data will be discarded.");
        }
    }

    const char *labels_resumable[3] = { "[Y] Yes", "[M] Minimize", "[N] No" };
    const char *labels_normal[2] = { "[Y] Yes", "[N] No (ESC)" };
    const char *labels_message[1] = { "[OK] (Enter)" };

    for (int i = 0; i < btn_count; i++) {
        const dialog_button_rect_t *b = &buttons[i];
        const bool is_focused = (i == dialog_state.selected_button);
        const char *label = dialog_state.type == SOLAR_OS_DIALOG_TYPE_MESSAGE ?
                            labels_message[i] :
                            (dialog_state.resumable ? labels_resumable[i] : labels_normal[i]);

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        if (is_focused) {
            solar_os_gfx_rect(gfx, b->x, b->y, b->w, b->h);
            solar_os_gfx_rect(gfx, b->x + 1, b->y + 1, b->w - 2, b->h - 2);
            solar_os_gfx_rect(gfx, b->x + 2, b->y + 2, b->w - 4, b->h - 4);
        } else {
            solar_os_gfx_rect(gfx, b->x, b->y, b->w, b->h);
        }

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        const size_t lw = solar_os_gfx_text_width(gfx, label);
        const int tx = b->x + (b->w - (int)lw) / 2;
        const int ty = b->y + 21;
        solar_os_gfx_text(gfx, tx, ty, label);
    }

    solar_os_gfx_present(gfx);
}

solar_os_dialog_action_t solar_os_dialog_handle_event(const solar_os_event_t *event)
{
    if (event == NULL || !dialog_state.active) {
        return SOLAR_OS_DIALOG_ACTION_NONE;
    }

    if (dialog_state.type == SOLAR_OS_DIALOG_TYPE_MESSAGE) {
        if (event->type == SOLAR_OS_EVENT_CLICK) {
            const int16_t px = event->data.click.x;
            const int16_t py = event->data.click.y;
            int dx, dy, dw, dh;
            dialog_button_rect_t buttons[3];
            int btn_count = 0;
            dialog_get_rects(400, 300, &dx, &dy, &dw, &dh, buttons, &btn_count);
            if (btn_count > 0 && px >= buttons[0].x && px < buttons[0].x + buttons[0].w &&
                py >= buttons[0].y && py < buttons[0].y + buttons[0].h) {
                return SOLAR_OS_DIALOG_ACTION_CANCEL;
            }
            if (px < dx || px >= dx + dw || py < dy || py >= dy + dh) {
                return SOLAR_OS_DIALOG_ACTION_CANCEL;
            }
            return SOLAR_OS_DIALOG_ACTION_NONE;
        }
        if (event->type == SOLAR_OS_EVENT_CHAR || event->type == SOLAR_OS_EVENT_KEY) {
            return SOLAR_OS_DIALOG_ACTION_CANCEL;
        }
        return SOLAR_OS_DIALOG_ACTION_NONE;
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        int dx, dy, dw, dh;
        dialog_button_rect_t buttons[3];
        int btn_count = 0;
        dialog_get_rects(400, 300, &dx, &dy, &dw, &dh, buttons, &btn_count);

        for (int i = 0; i < btn_count; i++) {
            const dialog_button_rect_t *b = &buttons[i];
            if (px >= b->x && px < b->x + b->w && py >= b->y && py < b->y + b->h) {
                if (dialog_state.resumable) {
                    if (i == 0) return SOLAR_OS_DIALOG_ACTION_QUIT;
                    if (i == 1) return SOLAR_OS_DIALOG_ACTION_MINIMIZE;
                    if (i == 2) return SOLAR_OS_DIALOG_ACTION_CANCEL;
                } else {
                    if (i == 0) return SOLAR_OS_DIALOG_ACTION_QUIT;
                    if (i == 1) return SOLAR_OS_DIALOG_ACTION_CANCEL;
                }
            }
        }

        if (px < dx || px >= dx + dw || py < dy || py >= dy + dh) {
            return SOLAR_OS_DIALOG_ACTION_CANCEL;
        }

        return SOLAR_OS_DIALOG_ACTION_NONE;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (ch == 'e' || ch == 'E' || ch == 'y' || ch == 'Y') {
            return SOLAR_OS_DIALOG_ACTION_QUIT;
        }

        if (ch == 'h' || ch == 'H' || ch == 'n' || ch == 'N' || ch == SOLAR_OS_KEY_ESCAPE) {
            return SOLAR_OS_DIALOG_ACTION_CANCEL;
        }

        if (ch == 'k' || ch == 'K' || ch == 'm' || ch == 'M' || ch == ' ') {
            if (dialog_state.resumable) {
                return SOLAR_OS_DIALOG_ACTION_MINIMIZE;
            } else {
                return SOLAR_OS_DIALOG_ACTION_CANCEL;
            }
        }

        if (ch == SOLAR_OS_KEY_LEFT) {
            if (dialog_state.selected_button > 0) dialog_state.selected_button--;
            return SOLAR_OS_DIALOG_ACTION_NONE;
        }
        if (ch == SOLAR_OS_KEY_RIGHT) {
            const int max_btn = dialog_state.resumable ? 2 : 1;
            if (dialog_state.selected_button < max_btn) dialog_state.selected_button++;
            return SOLAR_OS_DIALOG_ACTION_NONE;
        }

        if (ch == '\r' || ch == '\n') {
            if (dialog_state.resumable) {
                if (dialog_state.selected_button == 0) return SOLAR_OS_DIALOG_ACTION_QUIT;
                if (dialog_state.selected_button == 1) return SOLAR_OS_DIALOG_ACTION_MINIMIZE;
                if (dialog_state.selected_button == 2) return SOLAR_OS_DIALOG_ACTION_CANCEL;
            } else {
                if (dialog_state.selected_button == 0) return SOLAR_OS_DIALOG_ACTION_QUIT;
                if (dialog_state.selected_button == 1) return SOLAR_OS_DIALOG_ACTION_CANCEL;
            }
        }
    }

    return SOLAR_OS_DIALOG_ACTION_NONE;
}
