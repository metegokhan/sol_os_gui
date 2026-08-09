#include "solar_os_sensors.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "solar_os_board_caps.h"
#include "solar_os_stream.h"
#if SOLAR_OS_BOARD_HAS_TEMPERATURE || SOLAR_OS_BOARD_HAS_HUMIDITY
#include "solar_os_board_sensors.h"
#endif

static esp_err_t sensors_stream_read_scalar(
    void *user,
    const solar_os_stream_read_options_t *options,
    float *value)
{
    (void)options;
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_environment_t environment;
    const esp_err_t err = solar_os_sensors_read_environment(&environment);
    if (err == ESP_OK) {
        *value = (uintptr_t)user == 0U ?
            environment.temperature_c : environment.humidity_percent;
    }
    return err;
}

static esp_err_t sensors_register_stream(const char *id,
                                         const char *unit,
                                         const char *summary,
                                         uintptr_t field)
{
    solar_os_stream_driver_t driver = {
        .info = {
            .type = SOLAR_OS_STREAM_TYPE_SCALAR,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .sharing = SOLAR_OS_STREAM_SHARING_SHARED,
        },
        .read_scalar = sensors_stream_read_scalar,
        .user = (void *)field,
    };
    strlcpy(driver.info.id, id, sizeof(driver.info.id));
    strlcpy(driver.info.provider, "sensors", sizeof(driver.info.provider));
    strlcpy(driver.info.device, "environment0", sizeof(driver.info.device));
    strlcpy(driver.info.unit, unit, sizeof(driver.info.unit));
    strlcpy(driver.info.format, "f32", sizeof(driver.info.format));
    strlcpy(driver.info.summary, summary, sizeof(driver.info.summary));
    return solar_os_stream_register(&driver);
}

esp_err_t solar_os_sensors_init(void)
{
#if SOLAR_OS_BOARD_HAS_TEMPERATURE || SOLAR_OS_BOARD_HAS_HUMIDITY
    esp_err_t err = solar_os_board_sensors_init();
    if (err != ESP_OK) {
        return err;
    }
#if SOLAR_OS_BOARD_HAS_TEMPERATURE
    err = sensors_register_stream("temperature", "C", "ambient temperature", 0U);
    if (err != ESP_OK) {
        return err;
    }
#endif
#if SOLAR_OS_BOARD_HAS_HUMIDITY
    err = sensors_register_stream("humidity", "percent", "relative humidity", 1U);
    if (err != ESP_OK) {
#if SOLAR_OS_BOARD_HAS_TEMPERATURE
        (void)solar_os_stream_unregister("temperature");
#endif
        return err;
    }
#endif
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_sensors_read_environment(solar_os_environment_t *environment)
{
    if (environment == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !SOLAR_OS_BOARD_HAS_TEMPERATURE && !SOLAR_OS_BOARD_HAS_HUMIDITY
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_board_environment_t measurement;
    const esp_err_t ret = solar_os_board_sensors_read_environment(&measurement);
    if (ret != ESP_OK) {
        return ret;
    }

    environment->temperature_c = measurement.temperature_c;
    environment->humidity_percent = measurement.humidity_percent;
    return ESP_OK;
#endif
}
