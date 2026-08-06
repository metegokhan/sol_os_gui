#include "ps2_bus.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"

#define PS2_FRAME_GAP_US 2000LL

static unsigned ps2_ones(uint8_t value)
{
    unsigned count = 0;
    while (value != 0) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

static void ps2_frame_reset(solar_os_ps2_bus_t *bus)
{
    bus->frame_bit = 0;
    bus->frame_data = 0;
    bus->frame_parity = 0;
}

static void ps2_clock_isr(void *arg)
{
    solar_os_ps2_bus_t *bus = arg;
    if (bus == NULL || !bus->running) {
        return;
    }

    portENTER_CRITICAL_ISR(&bus->lock);
    const int64_t now_us = esp_timer_get_time();
    if (bus->last_edge_us != 0 && now_us - bus->last_edge_us > PS2_FRAME_GAP_US) {
        ps2_frame_reset(bus);
    }
    bus->last_edge_us = now_us;
    const bool high = gpio_get_level((gpio_num_t)bus->data_pin) != 0;

    if (bus->frame_bit == 0) {
        if (high) {
            bus->stats.frame_errors++;
            portEXIT_CRITICAL_ISR(&bus->lock);
            return;
        }
        bus->frame_bit = 1;
        portEXIT_CRITICAL_ISR(&bus->lock);
        return;
    }
    if (bus->frame_bit <= 8U) {
        if (high) {
            bus->frame_data |= (uint8_t)(1U << (bus->frame_bit - 1U));
        }
        bus->frame_bit++;
        portEXIT_CRITICAL_ISR(&bus->lock);
        return;
    }
    if (bus->frame_bit == 9U) {
        bus->frame_parity = high ? 1U : 0U;
        bus->frame_bit++;
        portEXIT_CRITICAL_ISR(&bus->lock);
        return;
    }

    const bool parity_ok = ((ps2_ones(bus->frame_data) + bus->frame_parity) & 1U) != 0;
    if (high && parity_ok) {
        const uint8_t next = (uint8_t)((bus->write_index + 1U) % SOLAR_OS_PS2_RX_BUFFER_SIZE);
        if (next == bus->read_index) {
            bus->stats.overruns++;
        } else {
            bus->rx_buffer[bus->write_index] = bus->frame_data;
            bus->write_index = next;
            bus->stats.bytes++;
        }
    } else {
        bus->stats.frame_errors++;
    }
    ps2_frame_reset(bus);
    portEXIT_CRITICAL_ISR(&bus->lock);
}

esp_err_t solar_os_ps2_bus_start(solar_os_ps2_bus_t *bus,
                                 const solar_os_bus_ps2_config_t *config)
{
    if (bus == NULL || config == NULL || config->clock_pin < 0 ||
        config->data_pin < 0 || config->clock_pin == config->data_pin) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(bus, 0, sizeof(*bus));
    bus->clock_pin = config->clock_pin;
    bus->data_pin = config->data_pin;
    bus->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

    const gpio_config_t data_config = {
        .pin_bit_mask = 1ULL << (unsigned)config->data_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&data_config);
    if (err != ESP_OK) {
        return err;
    }
    const gpio_config_t clock_config = {
        .pin_bit_mask = 1ULL << (unsigned)config->clock_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&clock_config);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        (void)gpio_set_intr_type((gpio_num_t)config->clock_pin, GPIO_INTR_DISABLE);
        return err;
    }
    bus->running = true;
    err = gpio_isr_handler_add((gpio_num_t)config->clock_pin, ps2_clock_isr, bus);
    if (err != ESP_OK) {
        bus->running = false;
        (void)gpio_set_intr_type((gpio_num_t)config->clock_pin, GPIO_INTR_DISABLE);
        return err;
    }
    return ESP_OK;
}

void solar_os_ps2_bus_stop(solar_os_ps2_bus_t *bus)
{
    if (bus == NULL || !bus->running) {
        return;
    }
    bus->running = false;
    (void)gpio_set_intr_type((gpio_num_t)bus->clock_pin, GPIO_INTR_DISABLE);
    (void)gpio_isr_handler_remove((gpio_num_t)bus->clock_pin);
}

size_t solar_os_ps2_bus_read(solar_os_ps2_bus_t *bus,
                             uint8_t *data,
                             size_t data_len)
{
    if (bus == NULL || data == NULL || data_len == 0) {
        return 0;
    }
    portENTER_CRITICAL(&bus->lock);
    size_t count = 0;
    while (count < data_len && bus->read_index != bus->write_index) {
        data[count++] = bus->rx_buffer[bus->read_index];
        bus->read_index = (uint8_t)((bus->read_index + 1U) % SOLAR_OS_PS2_RX_BUFFER_SIZE);
    }
    portEXIT_CRITICAL(&bus->lock);
    return count;
}

void solar_os_ps2_bus_get_stats(solar_os_ps2_bus_t *bus,
                                solar_os_ps2_bus_stats_t *stats)
{
    if (bus == NULL || stats == NULL) {
        return;
    }
    portENTER_CRITICAL(&bus->lock);
    *stats = bus->stats;
    portEXIT_CRITICAL(&bus->lock);
}
