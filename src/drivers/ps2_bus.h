#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "solar_os_bus_types.h"

#define SOLAR_OS_PS2_RX_BUFFER_SIZE 32U

typedef struct {
    uint32_t bytes;
    uint32_t frame_errors;
    uint32_t overruns;
} solar_os_ps2_bus_stats_t;

typedef struct {
    int clock_pin;
    int data_pin;
    volatile bool running;
    volatile uint8_t frame_bit;
    volatile uint8_t frame_data;
    volatile uint8_t frame_parity;
    volatile uint8_t read_index;
    volatile uint8_t write_index;
    volatile int64_t last_edge_us;
    uint8_t rx_buffer[SOLAR_OS_PS2_RX_BUFFER_SIZE];
    solar_os_ps2_bus_stats_t stats;
    portMUX_TYPE lock;
} solar_os_ps2_bus_t;

esp_err_t solar_os_ps2_bus_start(solar_os_ps2_bus_t *bus,
                                 const solar_os_bus_ps2_config_t *config);
void solar_os_ps2_bus_stop(solar_os_ps2_bus_t *bus);
size_t solar_os_ps2_bus_read(solar_os_ps2_bus_t *bus,
                             uint8_t *data,
                             size_t data_len);
void solar_os_ps2_bus_get_stats(solar_os_ps2_bus_t *bus,
                                solar_os_ps2_bus_stats_t *stats);
