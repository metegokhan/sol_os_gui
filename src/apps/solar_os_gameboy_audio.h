#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t solar_os_gameboy_audio_init(void);
esp_err_t solar_os_gameboy_audio_resume(void);
void solar_os_gameboy_audio_suspend(void);
void solar_os_gameboy_audio_reset(void);
void solar_os_gameboy_audio_deinit(void);

uint8_t solar_os_gameboy_audio_read(uint16_t address);
void solar_os_gameboy_audio_write(uint16_t address, uint8_t value);
