#pragma once

#include "esp_err.h"
#include "u8g2.h"

#define SOLAR_OS_VIRTUAL_DISPLAY_WIDTH 400U
#define SOLAR_OS_VIRTUAL_DISPLAY_HEIGHT 300U
#define SOLAR_OS_VIRTUAL_DISPLAY_NATIVE_WIDTH 300U
#define SOLAR_OS_VIRTUAL_DISPLAY_NATIVE_HEIGHT 400U

typedef struct solar_os_virtual_display solar_os_virtual_display_t;

esp_err_t solar_os_virtual_display_create(const char *name,
                                          solar_os_virtual_display_t **display);
esp_err_t solar_os_virtual_display_destroy(solar_os_virtual_display_t *display);
u8g2_t *solar_os_virtual_display_u8g2(solar_os_virtual_display_t *display);
