#include "rfm95.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_spi.h"

#define RFM95_FXOSC_HZ 32000000ULL
#define RFM95_FSTEP_DEN 524288ULL

#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FRF_MSB 0x06
#define REG_FRF_MID 0x07
#define REG_FRF_LSB 0x08
#define REG_PA_CONFIG 0x09
#define REG_OCP 0x0B
#define REG_LNA 0x0C
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_TX_BASE_ADDR 0x0E
#define REG_FIFO_RX_BASE_ADDR 0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS 0x12
#define REG_RX_NB_BYTES 0x13
#define REG_PKT_SNR_VALUE 0x19
#define REG_PKT_RSSI_VALUE 0x1A
#define REG_RSSI_VALUE 0x1B
#define REG_MODEM_CONFIG1 0x1D
#define REG_MODEM_CONFIG2 0x1E
#define REG_SYMB_TIMEOUT_LSB 0x1F
#define REG_PREAMBLE_MSB 0x20
#define REG_PREAMBLE_LSB 0x21
#define REG_PAYLOAD_LENGTH 0x22
#define REG_MAX_PAYLOAD_LENGTH 0x23
#define REG_MODEM_CONFIG3 0x26
#define REG_DETECT_OPTIMIZE 0x31
#define REG_DETECTION_THRESHOLD 0x37
#define REG_SYNC_WORD 0x39
#define REG_DIO_MAPPING1 0x40
#define REG_VERSION 0x42
#define REG_PA_DAC 0x4D

#define OP_MODE_LONG_RANGE 0x80
#define OP_MODE_LOW_FREQUENCY 0x08
#define OP_MODE_SLEEP 0x00
#define OP_MODE_STANDBY 0x01
#define OP_MODE_TX 0x03
#define OP_MODE_RX_CONTINUOUS 0x05

#define IRQ_RX_TIMEOUT 0x80
#define IRQ_RX_DONE 0x40
#define IRQ_PAYLOAD_CRC_ERROR 0x20
#define IRQ_TX_DONE 0x08

#define SPI_WRITE_BIT 0x80
#define RFM95_MODE_SETTLE_MS 1

static esp_err_t rfm95_lock(rfm95_t *dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev->mutex == NULL) {
        dev->mutex = xSemaphoreCreateMutex();
        if (dev->mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    return ESP_OK;
}

static void rfm95_unlock(rfm95_t *dev)
{
    if (dev != NULL && dev->mutex != NULL) {
        xSemaphoreGive(dev->mutex);
    }
}

static esp_err_t rfm95_transfer(rfm95_t *dev,
                                const uint8_t *tx,
                                uint8_t *rx,
                                size_t len)
{
    return solar_os_bus_spi_transfer(dev->spi_bus,
                                     dev->cs_pin,
                                     0,
                                     dev->speed_hz,
                                     tx,
                                     rx,
                                     len);
}

static esp_err_t rfm95_read_reg_locked(rfm95_t *dev, uint8_t reg, uint8_t *value)
{
    uint8_t tx[2] = {(uint8_t)(reg & 0x7F), 0};
    uint8_t rx[2] = {0};

    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = rfm95_transfer(dev, tx, rx, sizeof(tx));
    if (ret == ESP_OK) {
        *value = rx[1];
    }
    return ret;
}

static esp_err_t rfm95_write_reg_locked(rfm95_t *dev, uint8_t reg, uint8_t value)
{
    const uint8_t tx[2] = {(uint8_t)(reg | SPI_WRITE_BIT), value};
    return rfm95_transfer(dev, tx, NULL, sizeof(tx));
}

static esp_err_t rfm95_write_burst_locked(rfm95_t *dev,
                                          uint8_t reg,
                                          const uint8_t *data,
                                          size_t len)
{
    uint8_t tx[RFM95_MAX_PACKET_LEN + 1];

    if (data == NULL || len == 0 || len > RFM95_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    tx[0] = (uint8_t)(reg | SPI_WRITE_BIT);
    memcpy(&tx[1], data, len);
    return rfm95_transfer(dev, tx, NULL, len + 1);
}

static esp_err_t rfm95_read_burst_locked(rfm95_t *dev,
                                         uint8_t reg,
                                         uint8_t *data,
                                         size_t len)
{
    uint8_t tx[RFM95_MAX_PACKET_LEN + 1] = {0};
    uint8_t rx[RFM95_MAX_PACKET_LEN + 1] = {0};

    if (data == NULL || len == 0 || len > RFM95_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    tx[0] = (uint8_t)(reg & 0x7F);
    const esp_err_t ret = rfm95_transfer(dev, tx, rx, len + 1);
    if (ret == ESP_OK) {
        memcpy(data, &rx[1], len);
    }
    return ret;
}

static uint8_t rfm95_mode_bits(solar_os_radio_state_t state)
{
    switch (state) {
    case SOLAR_OS_RADIO_STATE_SLEEP:
        return OP_MODE_SLEEP;
    case SOLAR_OS_RADIO_STATE_STANDBY:
        return OP_MODE_STANDBY;
    case SOLAR_OS_RADIO_STATE_RX:
        return OP_MODE_RX_CONTINUOUS;
    case SOLAR_OS_RADIO_STATE_TX:
        return OP_MODE_TX;
    case SOLAR_OS_RADIO_STATE_UNKNOWN:
    default:
        return 0xFF;
    }
}

static uint8_t rfm95_op_mode(const rfm95_t *dev, solar_os_radio_state_t state)
{
    const bool low_frequency = dev != NULL && dev->config.frequency_hz < 525000000U;
    return (uint8_t)(OP_MODE_LONG_RANGE |
                     (low_frequency ? OP_MODE_LOW_FREQUENCY : 0) |
                     rfm95_mode_bits(state));
}

static esp_err_t rfm95_set_state_locked(rfm95_t *dev, solar_os_radio_state_t state)
{
    if (rfm95_mode_bits(state) == 0xFF) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = rfm95_write_reg_locked(dev, REG_OP_MODE, rfm95_op_mode(dev, state));
    if (ret == ESP_OK) {
        dev->state = state;
        if (state != SOLAR_OS_RADIO_STATE_SLEEP) {
            vTaskDelay(pdMS_TO_TICKS(RFM95_MODE_SETTLE_MS));
        }
    }
    return ret;
}

static solar_os_radio_state_t rfm95_state_from_op_mode(uint8_t op_mode)
{
    switch (op_mode & 0x07) {
    case OP_MODE_SLEEP:
        return SOLAR_OS_RADIO_STATE_SLEEP;
    case OP_MODE_STANDBY:
        return SOLAR_OS_RADIO_STATE_STANDBY;
    case OP_MODE_TX:
        return SOLAR_OS_RADIO_STATE_TX;
    case OP_MODE_RX_CONTINUOUS:
        return SOLAR_OS_RADIO_STATE_RX;
    default:
        return SOLAR_OS_RADIO_STATE_UNKNOWN;
    }
}

static bool rfm95_bandwidth_bits(uint32_t bandwidth_hz, uint8_t *bits)
{
    if (bits == NULL) {
        return false;
    }
    switch (bandwidth_hz) {
    case 125000:
        *bits = 7;
        return true;
    case 250000:
        *bits = 8;
        return true;
    case 500000:
        *bits = 9;
        return true;
    default:
        return false;
    }
}

static bool rfm95_config_valid(const solar_os_radio_config_t *config)
{
    uint8_t bandwidth = 0;

    if (config == NULL ||
        config->modulation != SOLAR_OS_RADIO_MODULATION_LORA ||
        config->frequency_hz < 862000000U ||
        config->frequency_hz > 1020000000U ||
        !rfm95_bandwidth_bits(config->rx_bandwidth_hz, &bandwidth) ||
        config->spreading_factor < 6 ||
        config->spreading_factor > 12 ||
        config->coding_rate_denominator < 5 ||
        config->coding_rate_denominator > 8 ||
        config->preamble_len < 6 ||
        config->sync_word_len != 1 ||
        config->tx_power_dbm < 2 ||
        config->tx_power_dbm > 20 ||
        config->payload_length == 0 ||
        config->payload_length > RFM95_MAX_PACKET_LEN ||
        config->has_node_id ||
        config->has_network_id) {
        return false;
    }
    if (config->spreading_factor == 6 && config->variable_length) {
        return false;
    }
    return true;
}

static uint32_t rfm95_frequency_reg(uint32_t frequency_hz)
{
    return (uint32_t)(((uint64_t)frequency_hz * RFM95_FSTEP_DEN +
                       RFM95_FXOSC_HZ / 2ULL) /
                      RFM95_FXOSC_HZ);
}

static bool rfm95_low_data_rate_optimize(const solar_os_radio_config_t *config)
{
    const uint64_t symbol_us =
        ((uint64_t)1U << config->spreading_factor) * 1000000ULL /
        config->rx_bandwidth_hz;
    return symbol_us > 16000ULL;
}

static esp_err_t rfm95_configure_power_locked(rfm95_t *dev, int8_t power_dbm)
{
    uint8_t output_power = 0;
    uint8_t pa_dac = 0x84;
    uint8_t ocp = 0x2B;

    if (power_dbm > 17) {
        output_power = (uint8_t)(power_dbm - 5);
        pa_dac = 0x87;
        ocp = 0x31;
    } else {
        output_power = (uint8_t)(power_dbm - 2);
    }

    esp_err_t ret = rfm95_write_reg_locked(dev,
                                           REG_PA_CONFIG,
                                           (uint8_t)(0x80 | (output_power & 0x0F)));
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_PA_DAC, pa_dac);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_OCP, ocp);
    }
    return ret;
}

static esp_err_t rfm95_wait_irq_locked(rfm95_t *dev,
                                       uint8_t mask,
                                       uint32_t timeout_ms,
                                       uint8_t *flags)
{
    const int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)timeout_ms * 1000;

    while (true) {
        uint8_t value = 0;
        const esp_err_t ret = rfm95_read_reg_locked(dev, REG_IRQ_FLAGS, &value);
        if (ret != ESP_OK) {
            return ret;
        }
        if ((value & mask) != 0) {
            if (flags != NULL) {
                *flags = value;
            }
            return ESP_OK;
        }
        if (timeout_ms == 0 || esp_timer_get_time() - start_us >= timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t rfm95_init(rfm95_t *dev,
                     const char *spi_bus,
                     int cs_pin,
                     uint32_t speed_hz)
{
    if (dev == NULL || spi_bus == NULL || spi_bus[0] == '\0' ||
        strnlen(spi_bus, sizeof(dev->spi_bus)) >= sizeof(dev->spi_bus) ||
        cs_pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev->mutex == NULL) {
        dev->mutex = xSemaphoreCreateMutex();
        if (dev->mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    strlcpy(dev->spi_bus, spi_bus, sizeof(dev->spi_bus));
    dev->cs_pin = cs_pin;
    dev->speed_hz = speed_hz != 0 ? speed_hz : SOLAR_OS_SPI_DEFAULT_SPEED_HZ;
    dev->state = SOLAR_OS_RADIO_STATE_UNKNOWN;
    return ESP_OK;
}

esp_err_t rfm95_probe(rfm95_t *dev, uint8_t *version)
{
    esp_err_t ret = rfm95_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t value = 0;
    ret = rfm95_read_reg_locked(dev, REG_VERSION, &value);
    rfm95_unlock(dev);
    if (ret != ESP_OK) {
        return ret;
    }
    if (version != NULL) {
        *version = value;
    }
    return value == RFM95_VERSION ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t rfm95_configure(rfm95_t *dev, const solar_os_radio_config_t *config)
{
    uint8_t bandwidth = 0;

    if (!rfm95_config_valid(config) ||
        !rfm95_bandwidth_bits(config->rx_bandwidth_hz, &bandwidth)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm95_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }

    /* LongRangeMode can only change while asleep. */
    ret = rfm95_write_reg_locked(dev, REG_OP_MODE, OP_MODE_SLEEP);
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_OP_MODE,
                                     (uint8_t)(OP_MODE_LONG_RANGE |
                                               (config->frequency_hz < 525000000U
                                                    ? OP_MODE_LOW_FREQUENCY
                                                    : 0) |
                                               OP_MODE_SLEEP));
    }

    const uint32_t frequency = rfm95_frequency_reg(config->frequency_hz);
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FRF_MSB, (uint8_t)(frequency >> 16));
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FRF_MID, (uint8_t)(frequency >> 8));
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FRF_LSB, (uint8_t)frequency);
    }
    if (ret == ESP_OK) {
        ret = rfm95_configure_power_locked(dev, config->tx_power_dbm);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_LNA, 0x23);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FIFO_TX_BASE_ADDR, 0x00);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FIFO_RX_BASE_ADDR, 0x00);
    }

    const uint8_t modem_config1 =
        (uint8_t)((bandwidth << 4) |
                  ((config->coding_rate_denominator - 4U) << 1) |
                  (config->variable_length ? 0 : 1));
    const uint8_t modem_config2 =
        (uint8_t)((config->spreading_factor << 4) |
                  (config->crc_enabled ? 0x04 : 0) |
                  0x03);
    const uint8_t modem_config3 =
        (uint8_t)(0x04 | (rfm95_low_data_rate_optimize(config) ? 0x08 : 0));

    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_MODEM_CONFIG1, modem_config1);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_MODEM_CONFIG2, modem_config2);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_MODEM_CONFIG3, modem_config3);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_SYMB_TIMEOUT_LSB, 0xFF);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_PREAMBLE_MSB,
                                     (uint8_t)(config->preamble_len >> 8));
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_PREAMBLE_LSB,
                                     (uint8_t)config->preamble_len);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_PAYLOAD_LENGTH,
                                     (uint8_t)config->payload_length);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_MAX_PAYLOAD_LENGTH, RFM95_MAX_PACKET_LEN);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_SYNC_WORD, config->sync_word[0]);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_DIO_MAPPING1, 0x00);
    }
    if (ret == ESP_OK) {
        const bool sf6 = config->spreading_factor == 6;
        ret = rfm95_write_reg_locked(dev, REG_DETECT_OPTIMIZE, sf6 ? 0x05 : 0x03);
        if (ret == ESP_OK) {
            ret = rfm95_write_reg_locked(dev, REG_DETECTION_THRESHOLD, sf6 ? 0x0C : 0x0A);
        }
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_IRQ_FLAGS, 0xFF);
    }
    if (ret == ESP_OK) {
        ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
    }
    if (ret == ESP_OK) {
        dev->config = *config;
        dev->has_last_packet = false;
    }

    rfm95_unlock(dev);
    return ret;
}

esp_err_t rfm95_set_state(rfm95_t *dev, solar_os_radio_state_t state)
{
    esp_err_t ret = rfm95_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = rfm95_set_state_locked(dev, state);
    rfm95_unlock(dev);
    return ret;
}

esp_err_t rfm95_get_status(rfm95_t *dev, solar_os_radio_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm95_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t op_mode = 0;
    uint8_t rssi = 0;
    ret = rfm95_read_reg_locked(dev, REG_OP_MODE, &op_mode);
    if (ret == ESP_OK) {
        ret = rfm95_read_reg_locked(dev, REG_RSSI_VALUE, &rssi);
    }
    if (ret == ESP_OK) {
        memset(status, 0, sizeof(*status));
        status->state = rfm95_state_from_op_mode(op_mode);
        status->config = dev->config;
        status->has_rssi = true;
        status->rssi_dbm = (int16_t)(-157 + rssi);
        status->has_snr = dev->has_last_packet;
        status->snr_db = dev->last_snr_db;
        dev->state = status->state;
    }

    rfm95_unlock(dev);
    return ret;
}

esp_err_t rfm95_send(rfm95_t *dev,
                     const solar_os_radio_packet_t *packet,
                     uint32_t timeout_ms)
{
    if (packet == NULL || packet->len == 0 || packet->len > RFM95_MAX_PACKET_LEN ||
        packet->has_source || packet->has_destination) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm95_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!dev->config.variable_length && packet->len != dev->config.payload_length) {
        rfm95_unlock(dev);
        return ESP_ERR_INVALID_SIZE;
    }

    ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_IRQ_FLAGS, 0xFF);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FIFO_ADDR_PTR, 0x00);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_PAYLOAD_LENGTH, (uint8_t)packet->len);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_burst_locked(dev, REG_FIFO, packet->data, packet->len);
    }
    if (ret == ESP_OK) {
        ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_TX);
    }
    if (ret == ESP_OK) {
        ret = rfm95_wait_irq_locked(dev, IRQ_TX_DONE, timeout_ms, NULL);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_IRQ_FLAGS, IRQ_TX_DONE);
    }

    const esp_err_t standby_ret =
        rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
    if (ret == ESP_OK) {
        ret = standby_ret;
    }
    rfm95_unlock(dev);
    return ret;
}

esp_err_t rfm95_receive(rfm95_t *dev,
                        solar_os_radio_packet_t *packet,
                        uint32_t timeout_ms)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm95_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }

    if (dev->state != SOLAR_OS_RADIO_STATE_RX) {
        ret = rfm95_write_reg_locked(dev, REG_IRQ_FLAGS, 0xFF);
        if (ret == ESP_OK) {
            ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_RX);
        }
    }

    uint8_t flags = 0;
    if (ret == ESP_OK) {
        ret = rfm95_wait_irq_locked(dev,
                                    IRQ_RX_DONE | IRQ_RX_TIMEOUT,
                                    timeout_ms,
                                    &flags);
    }
    if (ret != ESP_OK) {
        rfm95_unlock(dev);
        return ret;
    }
    if ((flags & IRQ_RX_TIMEOUT) != 0) {
        (void)rfm95_write_reg_locked(dev, REG_IRQ_FLAGS, IRQ_RX_TIMEOUT);
        rfm95_unlock(dev);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t len = 0;
    uint8_t fifo_addr = 0;
    uint8_t raw_snr = 0;
    uint8_t raw_rssi = 0;
    ret = rfm95_read_reg_locked(dev, REG_RX_NB_BYTES, &len);
    if (ret == ESP_OK) {
        ret = rfm95_read_reg_locked(dev, REG_FIFO_RX_CURRENT_ADDR, &fifo_addr);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FIFO_ADDR_PTR, fifo_addr);
    }
    if (ret == ESP_OK) {
        ret = rfm95_read_reg_locked(dev, REG_PKT_SNR_VALUE, &raw_snr);
    }
    if (ret == ESP_OK) {
        ret = rfm95_read_reg_locked(dev, REG_PKT_RSSI_VALUE, &raw_rssi);
    }
    if (ret == ESP_OK && len == 0) {
        ret = ESP_ERR_INVALID_SIZE;
    }

    memset(packet, 0, sizeof(*packet));
    if (ret == ESP_OK) {
        ret = rfm95_read_burst_locked(dev, REG_FIFO, packet->data, len);
    }
    if (ret == ESP_OK) {
        const int16_t snr_quarters = (int8_t)raw_snr;
        const int16_t snr_db = snr_quarters / 4;
        int16_t rssi_dbm = (int16_t)(-157 + raw_rssi);
        if (snr_quarters < 0) {
            rssi_dbm += snr_db;
        }

        packet->len = len;
        packet->has_rssi = true;
        packet->rssi_dbm = rssi_dbm;
        packet->has_snr = true;
        packet->snr_db = snr_db;
        packet->crc_ok = (flags & IRQ_PAYLOAD_CRC_ERROR) == 0;
        dev->last_rssi_dbm = rssi_dbm;
        dev->last_snr_db = snr_db;
        dev->has_last_packet = true;
    }

    const esp_err_t clear_ret =
        rfm95_write_reg_locked(dev,
                               REG_IRQ_FLAGS,
                               IRQ_RX_DONE | IRQ_RX_TIMEOUT | IRQ_PAYLOAD_CRC_ERROR);
    if (ret == ESP_OK) {
        ret = clear_ret;
    }
    rfm95_unlock(dev);
    return ret;
}
