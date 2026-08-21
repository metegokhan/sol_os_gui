/*
 * Solar OS - BLE Gamepad / Controller Service
 * Xbox, Switch Pro/Joy-Con, 8BitDo & Android Gamepad Support
 */

#include "solar_os_gamepad.h"

#include <math.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"

#define TAG "gamepad"

#define STICK_DEADZONE 25
#define STICK_REPEAT_INTERVAL_MS 180U

static solar_os_gamepad_state_t pad_state = {0};
static solar_os_gamepad_state_t prev_pad_state = {0};
static solar_os_input_source_t pad_input_source = SOLAR_OS_INPUT_SOURCE_INVALID;

static uint32_t next_stick_repeat_ms = 0;
static portMUX_TYPE pad_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t pad_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

esp_err_t solar_os_gamepad_init(void)
{
    portENTER_CRITICAL(&pad_lock);
    memset(&pad_state, 0, sizeof(pad_state));
    memset(&prev_pad_state, 0, sizeof(prev_pad_state));
    portEXIT_CRITICAL(&pad_lock);

    if (pad_input_source == SOLAR_OS_INPUT_SOURCE_INVALID) {
        (void)solar_os_input_source_open("ble-gamepad", &pad_input_source);
    }

    SOLAR_OS_LOGI(TAG, "Gamepad service initialized");
    return ESP_OK;
}

void solar_os_gamepad_set_connected(bool connected)
{
    portENTER_CRITICAL(&pad_lock);
    pad_state.connected = connected;
    if (!connected) {
        memset(&pad_state, 0, sizeof(pad_state));
        memset(&prev_pad_state, 0, sizeof(prev_pad_state));
    }
    portEXIT_CRITICAL(&pad_lock);
}

bool solar_os_gamepad_is_connected(void)
{
    bool conn;
    portENTER_CRITICAL(&pad_lock);
    conn = pad_state.connected;
    portEXIT_CRITICAL(&pad_lock);
    return conn;
}

void solar_os_gamepad_get_state(solar_os_gamepad_state_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&pad_lock);
    *out = pad_state;
    portEXIT_CRITICAL(&pad_lock);
}

static void send_gamepad_key(uint16_t physical_id, uint8_t key, bool pressed)
{
    if (key == 0 || pad_input_source == SOLAR_OS_INPUT_SOURCE_INVALID) return;

    const solar_os_input_key_action_t action = pressed ? SOLAR_OS_INPUT_KEY_PRESS : SOLAR_OS_INPUT_KEY_RELEASE;
    (void)solar_os_input_write_key(pad_input_source,
                                   physical_id,
                                   SOLAR_OS_INPUT_USAGE_NONE,
                                   key,
                                   0,
                                   action);
}

/* Decode standard 8-way Hat Switch (0=Neutral, 1=N, 2=NE, 3=E, 4=SE, 5=S, 6=SW, 7=W, 8=NW) */
static void decode_hat_switch(uint8_t hat, bool *up, bool *down, bool *left, bool *right)
{
    *up = false;
    *down = false;
    *left = false;
    *right = false;

    switch (hat) {
    case 1: *up = true; break;
    case 2: *up = true; *right = true; break;
    case 3: *right = true; break;
    case 4: *down = true; *right = true; break;
    case 5: *down = true; break;
    case 6: *down = true; *left = true; break;
    case 7: *left = true; break;
    case 8: *up = true; *left = true; break;
    default: break;
    }
}

void solar_os_gamepad_process_report(const uint8_t *data, uint16_t length)
{
    if (data == NULL || length < 4) return;

    const uint32_t now = pad_millis();
    solar_os_gamepad_state_t cur = {0};
    cur.connected = true;
    cur.last_active_ms = now;

    /* 1. Xbox Wireless Controller (BLE Mode - 16..17 bytes) */
    if (length >= 15) {
        /* Stick LX: data[0..1] uint16 LE (0..65535, center 32768) */
        uint16_t raw_lx = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        uint16_t raw_ly = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
        uint16_t raw_rx = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
        uint16_t raw_ry = (uint16_t)data[6] | ((uint16_t)data[7] << 8);

        cur.stick_lx = (int8_t)(((int32_t)raw_lx - 32768) * 100 / 32768);
        cur.stick_ly = (int8_t)(((int32_t)raw_ly - 32768) * 100 / 32768);
        cur.stick_rx = (int8_t)(((int32_t)raw_rx - 32768) * 100 / 32768);
        cur.stick_ry = (int8_t)(((int32_t)raw_ry - 32768) * 100 / 32768);

        /* Triggers for Xbox (LT = data[8..9], RT = data[10..11]) */
        uint16_t raw_lt = (uint16_t)data[8] | ((uint16_t)data[9] << 8);
        uint16_t raw_rt = (uint16_t)data[10] | ((uint16_t)data[11] << 8);
        cur.btn_l2 = (raw_lt > 250);
        cur.btn_r2 = (raw_rt > 250);

        /* Hat Switch: byte 12 (or byte 14 depending on report ID) */
        uint8_t hat = (data[12] <= 8) ? data[12] : ((length > 14 && data[14] <= 8) ? data[14] : 0);
        decode_hat_switch(hat, &cur.dpad_up, &cur.dpad_down, &cur.dpad_left, &cur.dpad_right);

        /* Buttons Byte 1: A (bit 0), B (bit 1), X (bit 3), Y (bit 4), LB (bit 6), RB (bit 7) */
        uint8_t b1 = (data[13] != 0 || data[12] <= 8) ? data[13] : data[11];
        cur.btn_a = (b1 & 0x01) != 0;
        cur.btn_b = (b1 & 0x02) != 0;
        cur.btn_x = (b1 & 0x08) != 0;
        cur.btn_y = (b1 & 0x10) != 0;
        cur.btn_l1 = (b1 & 0x40) != 0;
        cur.btn_r1 = (b1 & 0x80) != 0;

        /* Buttons Byte 2: View/Select (bit 2), Menu/Start (bit 3), Xbox (bit 4) */
        if (length >= 15) {
            uint8_t b2 = data[14];
            cur.btn_select = (b2 & 0x04) != 0;
            cur.btn_start = (b2 & 0x08) != 0;
            cur.btn_menu = (b2 & 0x10) != 0;
        }
    }
    /* 2. Standard Android / 8BitDo / PC BLE Gamepad (4..12 bytes) */
    else {
        /* Standard 8-bit analog axes centered at 128 */
        cur.stick_lx = (int8_t)(((int)data[0] - 128) * 100 / 128);
        cur.stick_ly = (int8_t)(((int)data[1] - 128) * 100 / 128);
        if (length >= 4) {
            cur.stick_rx = (int8_t)(((int)data[2] - 128) * 100 / 128);
            cur.stick_ry = (int8_t)(((int)data[3] - 128) * 100 / 128);
        }

        /* Buttons / Hat */
        if (length >= 5) {
            uint8_t b1 = data[4];
            uint8_t hat = b1 & 0x0F;
            if (hat <= 8) {
                decode_hat_switch(hat, &cur.dpad_up, &cur.dpad_down, &cur.dpad_left, &cur.dpad_right);
            }
            cur.btn_a = (b1 & 0x10) != 0;
            cur.btn_b = (b1 & 0x20) != 0;
            cur.btn_x = (b1 & 0x40) != 0;
            cur.btn_y = (b1 & 0x80) != 0;
        }
        if (length >= 6) {
            uint8_t b2 = data[5];
            cur.btn_l1 = (b2 & 0x01) != 0;
            cur.btn_r1 = (b2 & 0x02) != 0;
            cur.btn_l2 = (b2 & 0x04) != 0;
            cur.btn_r2 = (b2 & 0x08) != 0;
            cur.btn_select = (b2 & 0x10) != 0;
            cur.btn_start = (b2 & 0x20) != 0;
            cur.btn_menu = (b2 & 0x40) != 0;
        }
    }

    /* Merge Left Joystick into D-Pad if beyond deadzone (50) */
    if (cur.stick_ly < -50) cur.dpad_up = true;
    if (cur.stick_ly > 50) cur.dpad_down = true;
    if (cur.stick_lx < -50) cur.dpad_left = true;
    if (cur.stick_lx > 50) cur.dpad_right = true;

    /* Right Joystick Custom Buttons */
    bool rstick_up = (cur.stick_ry < -50);
    bool rstick_down = (cur.stick_ry > 50);
    bool rstick_left = (cur.stick_rx < -50);
    bool rstick_right = (cur.stick_rx > 50);

    bool prev_rstick_up = (prev_pad_state.stick_ry < -50);
    bool prev_rstick_down = (prev_pad_state.stick_ry > 50);
    bool prev_rstick_left = (prev_pad_state.stick_rx < -50);
    bool prev_rstick_right = (prev_pad_state.stick_rx > 50);

    /* -------------------------------------------------------------
     * Dispatch State Changes to Solar OS Input Subsystem
     * ----------------------------------------------------------- */
    portENTER_CRITICAL(&pad_lock);
    solar_os_gamepad_state_t prev = prev_pad_state;
    pad_state = cur;
    prev_pad_state = cur;
    portEXIT_CRITICAL(&pad_lock);

    /* D-pad Directional Navigation -> Arrow Keys */
    if (cur.dpad_up != prev.dpad_up) send_gamepad_key(1, SOLAR_OS_KEY_UP, cur.dpad_up);
    if (cur.dpad_down != prev.dpad_down) send_gamepad_key(2, SOLAR_OS_KEY_DOWN, cur.dpad_down);
    if (cur.dpad_left != prev.dpad_left) send_gamepad_key(3, SOLAR_OS_KEY_LEFT, cur.dpad_left);
    if (cur.dpad_right != prev.dpad_right) send_gamepad_key(4, SOLAR_OS_KEY_RIGHT, cur.dpad_right);

    /* Right Stick -> Volume and Save/Load */
    if (rstick_up != prev_rstick_up) send_gamepad_key(25, SOLAR_OS_KEY_AUDIO_VOLUME_UP, rstick_up);
    if (rstick_down != prev_rstick_down) send_gamepad_key(26, SOLAR_OS_KEY_AUDIO_VOLUME_DOWN, rstick_down);
    if (rstick_left != prev_rstick_left) send_gamepad_key(27, SOLAR_OS_KEY_F5, rstick_left);
    if (rstick_right != prev_rstick_right) send_gamepad_key(28, SOLAR_OS_KEY_F7, rstick_right);

    /* Face Buttons: a:a, b:b, x:x, y:y */
    if (cur.btn_a != prev.btn_a) send_gamepad_key(5, 'a', cur.btn_a);
    if (cur.btn_b != prev.btn_b) send_gamepad_key(6, 'b', cur.btn_b);
    if (cur.btn_x != prev.btn_x) send_gamepad_key(7, 'x', cur.btn_x);
    if (cur.btn_y != prev.btn_y) send_gamepad_key(8, 'y', cur.btn_y);

    /* Shoulder / Triggers: rb:enter, lb:esc, rt:space, lt:tab */
    if (cur.btn_r1 != prev.btn_r1) send_gamepad_key(9, SOLAR_OS_KEY_ENTER, cur.btn_r1);     /* RB -> Enter */
    if (cur.btn_l1 != prev.btn_l1) send_gamepad_key(10, SOLAR_OS_KEY_ESCAPE, cur.btn_l1);   /* LB -> Esc */
    if (cur.btn_r2 != prev.btn_r2) send_gamepad_key(11, ' ', cur.btn_r2);                    /* RT -> Space */
    if (cur.btn_l2 != prev.btn_l2) send_gamepad_key(12, '\t', cur.btn_l2);                   /* LT -> Tab */

    /* Start / Select */
    if (cur.btn_start != prev.btn_start) send_gamepad_key(13, SOLAR_OS_KEY_ENTER, cur.btn_start);
    if (cur.btn_select != prev.btn_select) send_gamepad_key(14, SOLAR_OS_KEY_ESCAPE, cur.btn_select);
}

/* Handle continuous analog stick navigation with pacing */
void solar_os_gamepad_tick(uint32_t now_ms)
{
    /* Analog sticks are now processed in solar_os_gamepad_process_report 
       as continuous button holds natively. No need for pulsed ticks. */
    (void)now_ms;
}
