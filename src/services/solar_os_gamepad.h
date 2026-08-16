/*
 * Solar OS - BLE Gamepad / Controller Service
 * Xbox, Switch Pro/Joy-Con, 8BitDo & Android Gamepad Support
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool connected;
    /* D-pad direction states */
    bool dpad_up;
    bool dpad_down;
    bool dpad_left;
    bool dpad_right;

    /* Face buttons */
    bool btn_a;
    bool btn_b;
    bool btn_x;
    bool btn_y;

    /* Shoulder & Triggers */
    bool btn_l1;
    bool btn_r1;
    bool btn_l2;
    bool btn_r2;

    /* Special buttons */
    bool btn_select;
    bool btn_start;
    bool btn_menu;

    /* Analog stick values (-100 to 100) */
    int8_t stick_lx;
    int8_t stick_ly;
    int8_t stick_rx;
    int8_t stick_ry;

    uint32_t last_active_ms;
} solar_os_gamepad_state_t;

esp_err_t solar_os_gamepad_init(void);
void solar_os_gamepad_set_connected(bool connected);
bool solar_os_gamepad_is_connected(void);
void solar_os_gamepad_process_report(const uint8_t *data, uint16_t length);
void solar_os_gamepad_get_state(solar_os_gamepad_state_t *out);
void solar_os_gamepad_tick(uint32_t now_ms);
