#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "solar_os_gfx.h"
#include "solar_os.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOLAR_OS_DIALOG_ACTION_NONE = 0,
    SOLAR_OS_DIALOG_ACTION_QUIT,
    SOLAR_OS_DIALOG_ACTION_CANCEL,
    SOLAR_OS_DIALOG_ACTION_MINIMIZE,
} solar_os_dialog_action_t;

void solar_os_dialog_show_exit(const char *app_name, bool resumable);
bool solar_os_dialog_is_active(void);
void solar_os_dialog_close(void);
void solar_os_dialog_draw(solar_os_gfx_t *gfx);
solar_os_dialog_action_t solar_os_dialog_handle_event(const solar_os_event_t *event);

#ifdef __cplusplus
}
#endif