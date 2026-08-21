/*
 * Solar OS - Raw pulse capture / replay (RMT-based).
 *
 * Records and replays digital pulse trains on a free GPIO using the ESP32
 * RMT peripheral. Suited to IR remotes (via a 38 kHz demodulating receiver)
 * and 433 MHz OOK remotes (via an ASK/OOK receiver module), and replay via an
 * IR LED / 433 OOK transmitter. Timing is captured in microseconds.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t level;         /* signal level during the pulse: 0 or 1 */
    uint32_t duration_us;  /* pulse width in microseconds */
} solar_os_capture_pulse_t;

/*
 * One-shot capture of a pulse train on `pin` (must be a runtime-allowed input).
 * Waits up to `timeout_ms` for a frame; reception ends when a level stays put
 * longer than `idle_us` (the inter-frame gap) or the buffer fills. Fills up to
 * `max` pulses and writes the count to `out_count`.
 */
esp_err_t solar_os_capture_record(int pin,
                                  uint32_t timeout_ms,
                                  uint32_t idle_us,
                                  solar_os_capture_pulse_t *out,
                                  size_t max,
                                  size_t *out_count);

/*
 * Replays a pulse train on `pin` (output). `carrier_hz` > 0 modulates the high
 * levels with a carrier (use 38000 for most IR); 0 emits plain levels (for a
 * 433 MHz OOK transmitter).
 */
esp_err_t solar_os_capture_send(int pin,
                                const solar_os_capture_pulse_t *pulses,
                                size_t count,
                                uint32_t carrier_hz);

#ifdef __cplusplus
}
#endif
