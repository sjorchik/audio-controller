#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "bsp.h"

#define TDA7318_I2C_ADDR 0x44

esp_err_t tda7318_init(void);
esp_err_t tda7318_select_source(bsp_audio_source_t source);
esp_err_t tda7318_set_mute(bool mute);
