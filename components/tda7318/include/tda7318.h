// Файл: components/tda7318/include/tda7318.h
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "bsp.h"
#include "driver/i2c_master.h"

#define TDA7318_I2C_ADDR 0x44

typedef struct { uint32_t i2c_errors; } tda7318_stats_t;

esp_err_t tda7318_init(void);
esp_err_t tda7318_select_source(bsp_audio_source_t source);
esp_err_t tda7318_set_mute(bool mute);
esp_err_t tda7318_switch_source(bsp_audio_source_t source);
void tda7318_get_stats(tda7318_stats_t *stats);
i2c_master_bus_handle_t tda7318_get_i2c_bus(void);
esp_err_t tda7318_raw_write(uint8_t cmd);

bsp_audio_source_t tda7318_get_source(void);