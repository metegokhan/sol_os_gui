#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_gfx.h"

typedef struct solar_os_cassette_widget solar_os_cassette_widget_t;

esp_err_t solar_os_cassette_widget_create(solar_os_cassette_widget_t **widget);
void solar_os_cassette_widget_destroy(solar_os_cassette_widget_t *widget);
void solar_os_cassette_widget_reset(solar_os_cassette_widget_t *widget);
void solar_os_cassette_widget_update(solar_os_cassette_widget_t *widget,
                                     bool playing,
                                     uint32_t elapsed_ms,
                                     uint32_t total_ms,
                                     uint32_t now_ms);
void solar_os_cassette_widget_draw(solar_os_cassette_widget_t *widget,
                                   solar_os_gfx_t *gfx,
                                   int x,
                                   int y,
                                   int width,
                                   int height);
