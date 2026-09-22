#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bsp.h"

typedef struct {
    bsp_audio_source_t source;
    int8_t volume_db;
    int8_t source_trim_db[BSP_AUDIO_SOURCE_MAX];
    bool display_backlight_on;
} settings_t;

esp_err_t settings_init(void);
esp_err_t settings_load(settings_t *out_settings);
esp_err_t settings_save(const settings_t *settings);
