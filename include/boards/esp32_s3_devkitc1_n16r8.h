#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "solar_os_bus_types.h"
#include "solar_os_pin_types.h"

#define SOLAR_OS_BOARD_ID "esp32_s3_devkitc1_n16r8"
#define SOLAR_OS_BOARD_NAME "Espressif ESP32-S3-DevKitC-1-N16R8"
#define SOLAR_OS_BOARD_VENDOR "Espressif"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-S3-WROOM-1-N16R8"

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_43
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_44

#define SOLAR_OS_BOARD_I2C_PORT I2C_NUM_0
#define SOLAR_OS_BOARD_PIN_I2C_SDA GPIO_NUM_8
#define SOLAR_OS_BOARD_PIN_I2C_SCL GPIO_NUM_9

#define SOLAR_OS_BOARD_SPI_HOST SPI2_HOST
#define SOLAR_OS_BOARD_SPI_NAME "FSPI"
#define SOLAR_OS_BOARD_PIN_SPI_SCLK GPIO_NUM_12
#define SOLAR_OS_BOARD_PIN_SPI_MISO GPIO_NUM_13
#define SOLAR_OS_BOARD_PIN_SPI_MOSI GPIO_NUM_11
#define SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ 4096
#define SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK (1U << SPI3_HOST)
#define SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK ((1U << UART_NUM_1) | (1U << UART_NUM_2))
#define SOLAR_OS_BOARD_SPI_CS_SLOTS { \
    {.pin = GPIO_NUM_4, .name = "gpio4"}, \
    {.pin = GPIO_NUM_10, .name = "gpio10"}, \
    {.pin = GPIO_NUM_5, .name = "gpio5"}, \
    {.pin = GPIO_NUM_6, .name = "gpio6"}, \
    {.pin = GPIO_NUM_7, .name = "gpio7"}, \
}
#define SOLAR_OS_BOARD_BUSES { \
    { \
        .name = "i2c0", \
        .protocol = SOLAR_OS_BUS_PROTOCOL_I2C, \
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD, \
        .sharing = SOLAR_OS_BUS_SHARED, \
        .config.i2c = { \
            .port = SOLAR_OS_BOARD_I2C_PORT, \
            .sda_pin = SOLAR_OS_BOARD_PIN_I2C_SDA, \
            .scl_pin = SOLAR_OS_BOARD_PIN_I2C_SCL, \
            .speed_hz = SOLAR_OS_BUS_I2C_DEFAULT_SPEED_HZ, \
        }, \
    }, \
    { \
        .name = "spi0", \
        .protocol = SOLAR_OS_BUS_PROTOCOL_SPI, \
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD, \
        .sharing = SOLAR_OS_BUS_SHARED, \
        .config.spi = { \
            .host = SOLAR_OS_BOARD_SPI_HOST, \
            .sclk_pin = SOLAR_OS_BOARD_PIN_SPI_SCLK, \
            .miso_pin = SOLAR_OS_BOARD_PIN_SPI_MISO, \
            .mosi_pin = SOLAR_OS_BOARD_PIN_SPI_MOSI, \
            .max_transfer_size = SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ, \
            .cs_count = 5, \
            .cs = { \
                {.name = "gpio4", .pin = GPIO_NUM_4}, \
                {.name = "gpio10", .pin = GPIO_NUM_10}, \
                {.name = "gpio5", .pin = GPIO_NUM_5}, \
                {.name = "gpio6", .pin = GPIO_NUM_6}, \
                {.name = "gpio7", .pin = GPIO_NUM_7}, \
            }, \
        }, \
    }, \
    { \
        .name = "uart0", \
        .protocol = SOLAR_OS_BUS_PROTOCOL_UART, \
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD, \
        .sharing = SOLAR_OS_BUS_EXCLUSIVE, \
        .config.uart = { \
            .port = SOLAR_OS_BOARD_UART_PORT, \
            .tx_pin = SOLAR_OS_BOARD_PIN_UART_TX, \
            .rx_pin = SOLAR_OS_BOARD_PIN_UART_RX, \
            .baud_rate = SOLAR_OS_BUS_UART_DEFAULT_BAUD_RATE, \
        }, \
    }, \
}

#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_TITLE "J1 / J3 pin headers"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_VIEW \
    "component side; antenna at top, USB connectors at bottom"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_ROWS 22
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_COLUMNS 2
#define SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT 44
#define SOLAR_OS_BOARD_CONNECTOR_PINS { \
    {.connector = "J1", .position = 1, .row = 0, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "3V3"}, \
    {.connector = "J3", .position = 1, .row = 0, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "J1", .position = 2, .row = 1, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "3V3"}, \
    {.connector = "J3", .position = 2, .row = 1, .column = 1, .pin = GPIO_NUM_43, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "TX"}, \
    {.connector = "J1", .position = 3, .row = 2, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_CONTROL, .label = "RST"}, \
    {.connector = "J3", .position = 3, .row = 2, .column = 1, .pin = GPIO_NUM_44, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "RX"}, \
    {.connector = "J1", .position = 4, .row = 3, .column = 0, .pin = GPIO_NUM_4, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO4"}, \
    {.connector = "J3", .position = 4, .row = 3, .column = 1, .pin = GPIO_NUM_1, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO1"}, \
    {.connector = "J1", .position = 5, .row = 4, .column = 0, .pin = GPIO_NUM_5, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO5"}, \
    {.connector = "J3", .position = 5, .row = 4, .column = 1, .pin = GPIO_NUM_2, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO2"}, \
    {.connector = "J1", .position = 6, .row = 5, .column = 0, .pin = GPIO_NUM_6, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO6"}, \
    {.connector = "J3", .position = 6, .row = 5, .column = 1, .pin = GPIO_NUM_42, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO42"}, \
    {.connector = "J1", .position = 7, .row = 6, .column = 0, .pin = GPIO_NUM_7, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO7"}, \
    {.connector = "J3", .position = 7, .row = 6, .column = 1, .pin = GPIO_NUM_41, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO41"}, \
    {.connector = "J1", .position = 8, .row = 7, .column = 0, .pin = GPIO_NUM_15, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO15"}, \
    {.connector = "J3", .position = 8, .row = 7, .column = 1, .pin = GPIO_NUM_40, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO40"}, \
    {.connector = "J1", .position = 9, .row = 8, .column = 0, .pin = GPIO_NUM_16, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO16"}, \
    {.connector = "J3", .position = 9, .row = 8, .column = 1, .pin = GPIO_NUM_39, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO39"}, \
    {.connector = "J1", .position = 10, .row = 9, .column = 0, .pin = GPIO_NUM_17, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO17"}, \
    {.connector = "J3", .position = 10, .row = 9, .column = 1, .pin = GPIO_NUM_38, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO38"}, \
    {.connector = "J1", .position = 11, .row = 10, .column = 0, .pin = GPIO_NUM_18, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO18"}, \
    {.connector = "J3", .position = 11, .row = 10, .column = 1, .pin = GPIO_NUM_37, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO37"}, \
    {.connector = "J1", .position = 12, .row = 11, .column = 0, .pin = GPIO_NUM_8, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO8"}, \
    {.connector = "J3", .position = 12, .row = 11, .column = 1, .pin = GPIO_NUM_36, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO36"}, \
    {.connector = "J1", .position = 13, .row = 12, .column = 0, .pin = GPIO_NUM_3, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO3"}, \
    {.connector = "J3", .position = 13, .row = 12, .column = 1, .pin = GPIO_NUM_35, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO35"}, \
    {.connector = "J1", .position = 14, .row = 13, .column = 0, .pin = GPIO_NUM_46, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO46"}, \
    {.connector = "J3", .position = 14, .row = 13, .column = 1, .pin = GPIO_NUM_0, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO0"}, \
    {.connector = "J1", .position = 15, .row = 14, .column = 0, .pin = GPIO_NUM_9, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO9"}, \
    {.connector = "J3", .position = 15, .row = 14, .column = 1, .pin = GPIO_NUM_45, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO45"}, \
    {.connector = "J1", .position = 16, .row = 15, .column = 0, .pin = GPIO_NUM_10, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO10"}, \
    {.connector = "J3", .position = 16, .row = 15, .column = 1, .pin = GPIO_NUM_48, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO48"}, \
    {.connector = "J1", .position = 17, .row = 16, .column = 0, .pin = GPIO_NUM_11, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO11"}, \
    {.connector = "J3", .position = 17, .row = 16, .column = 1, .pin = GPIO_NUM_47, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO47"}, \
    {.connector = "J1", .position = 18, .row = 17, .column = 0, .pin = GPIO_NUM_12, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO12"}, \
    {.connector = "J3", .position = 18, .row = 17, .column = 1, .pin = GPIO_NUM_21, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO21"}, \
    {.connector = "J1", .position = 19, .row = 18, .column = 0, .pin = GPIO_NUM_13, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO13"}, \
    {.connector = "J3", .position = 19, .row = 18, .column = 1, .pin = GPIO_NUM_20, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO20"}, \
    {.connector = "J1", .position = 20, .row = 19, .column = 0, .pin = GPIO_NUM_14, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO14"}, \
    {.connector = "J3", .position = 20, .row = 19, .column = 1, .pin = GPIO_NUM_19, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO19"}, \
    {.connector = "J1", .position = 21, .row = 20, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "5V"}, \
    {.connector = "J3", .position = 21, .row = 20, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "J1", .position = 22, .row = 21, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "J3", .position = 22, .row = 21, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
}

#define SOLAR_OS_BOARD_EXPANSION_GPIO_MASK ((1ULL << GPIO_NUM_0) | \
                                            (1ULL << GPIO_NUM_1) | \
                                            (1ULL << GPIO_NUM_2) | \
                                            (1ULL << GPIO_NUM_3) | \
                                            (1ULL << GPIO_NUM_4) | \
                                            (1ULL << GPIO_NUM_5) | \
                                            (1ULL << GPIO_NUM_6) | \
                                            (1ULL << GPIO_NUM_7) | \
                                            (1ULL << GPIO_NUM_8) | \
                                            (1ULL << GPIO_NUM_9) | \
                                            (1ULL << GPIO_NUM_10) | \
                                            (1ULL << GPIO_NUM_11) | \
                                            (1ULL << GPIO_NUM_12) | \
                                            (1ULL << GPIO_NUM_13) | \
                                            (1ULL << GPIO_NUM_14) | \
                                            (1ULL << GPIO_NUM_15) | \
                                            (1ULL << GPIO_NUM_16) | \
                                            (1ULL << GPIO_NUM_17) | \
                                            (1ULL << GPIO_NUM_18) | \
                                            (1ULL << GPIO_NUM_19) | \
                                            (1ULL << GPIO_NUM_20) | \
                                            (1ULL << GPIO_NUM_21) | \
                                            (1ULL << GPIO_NUM_35) | \
                                            (1ULL << GPIO_NUM_36) | \
                                            (1ULL << GPIO_NUM_37) | \
                                            (1ULL << GPIO_NUM_38) | \
                                            (1ULL << GPIO_NUM_39) | \
                                            (1ULL << GPIO_NUM_40) | \
                                            (1ULL << GPIO_NUM_41) | \
                                            (1ULL << GPIO_NUM_42) | \
                                            (1ULL << GPIO_NUM_43) | \
                                            (1ULL << GPIO_NUM_44) | \
                                            (1ULL << GPIO_NUM_45) | \
                                            (1ULL << GPIO_NUM_46) | \
                                            (1ULL << GPIO_NUM_47) | \
                                            (1ULL << GPIO_NUM_48))
#define SOLAR_OS_BOARD_USER_GPIO_MASK ((1ULL << GPIO_NUM_1) | \
                                       (1ULL << GPIO_NUM_2) | \
                                       (1ULL << GPIO_NUM_4) | \
                                       (1ULL << GPIO_NUM_5) | \
                                       (1ULL << GPIO_NUM_6) | \
                                       (1ULL << GPIO_NUM_7) | \
                                       (1ULL << GPIO_NUM_10) | \
                                       (1ULL << GPIO_NUM_14) | \
                                       (1ULL << GPIO_NUM_15) | \
                                       (1ULL << GPIO_NUM_16) | \
                                       (1ULL << GPIO_NUM_17) | \
                                       (1ULL << GPIO_NUM_18) | \
                                       (1ULL << GPIO_NUM_21) | \
                                       (1ULL << GPIO_NUM_39) | \
                                       (1ULL << GPIO_NUM_40) | \
                                       (1ULL << GPIO_NUM_41) | \
                                       (1ULL << GPIO_NUM_42) | \
                                       (1ULL << GPIO_NUM_47))
#define SOLAR_OS_BOARD_EXPANSION_GPIO_LIST "0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 35 36 37 38 39 40 41 42 43 44 45 46 47 48"
#define SOLAR_OS_BOARD_USER_GPIO_LIST "1 2 4 5 6 7 10 14 15 16 17 18 21 39 40 41 42 47"
#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK ((1ULL << GPIO_NUM_1) | \
                                           (1ULL << GPIO_NUM_2) | \
                                           (1ULL << GPIO_NUM_4) | \
                                           (1ULL << GPIO_NUM_5) | \
                                           (1ULL << GPIO_NUM_6) | \
                                           (1ULL << GPIO_NUM_7) | \
                                           (1ULL << GPIO_NUM_10) | \
                                           (1ULL << GPIO_NUM_14) | \
                                           (1ULL << GPIO_NUM_15) | \
                                           (1ULL << GPIO_NUM_16) | \
                                           (1ULL << GPIO_NUM_17) | \
                                           (1ULL << GPIO_NUM_18))
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK SOLAR_OS_BOARD_USER_GPIO_MASK
#define SOLAR_OS_BOARD_GPIO_SLOTS { \
    {.pin = 0, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "BOOT/download"}, \
    {.pin = 1, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 2, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 3, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "strapping"}, \
    {.pin = 4, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / SPI CS"}, \
    {.pin = 5, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / SPI CS"}, \
    {.pin = 6, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / SPI CS"}, \
    {.pin = 7, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / SPI CS"}, \
    {.pin = 8, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "I2C SDA"}, \
    {.pin = 9, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "I2C SCL"}, \
    {.pin = 10, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / SPI CS"}, \
    {.pin = 11, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SPI MOSI"}, \
    {.pin = 12, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SPI SCK"}, \
    {.pin = 13, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SPI MISO"}, \
    {.pin = 14, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 15, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 16, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 17, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 18, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 19, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "USB D-/CDC"}, \
    {.pin = 20, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "USB D+/CDC"}, \
    {.pin = 21, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 35, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "Octal PSRAM"}, \
    {.pin = 36, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "Octal PSRAM"}, \
    {.pin = 37, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "Octal PSRAM"}, \
    {.pin = 38, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "RGB LED (v1.1)"}, \
    {.pin = 39, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / JTAG MTCK"}, \
    {.pin = 40, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / JTAG MTDO"}, \
    {.pin = 41, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / JTAG MTDI"}, \
    {.pin = 42, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / JTAG MTMS"}, \
    {.pin = 43, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "UART TX"}, \
    {.pin = 44, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "UART RX"}, \
    {.pin = 45, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "strapping"}, \
    {.pin = 46, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "strapping"}, \
    {.pin = 47, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 48, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "RGB LED (v1.0)"}, \
}
