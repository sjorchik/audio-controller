#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bsp.h"

#define AUDIO_VOLUME_MIN_DB (-90)
#define AUDIO_VOLUME_MAX_DB 0

esp_err_t audio_init(void);
esp_err_t audio_start(void);
esp_err_t audio_stop(void);
esp_err_t audio_set_source(bsp_audio_source_t source);
esp_err_t audio_set_volume_db(int8_t volume_db);
esp_err_t audio_set_source_trim_db(bsp_audio_source_t source, int8_t trim_db);
esp_err_t audio_apply_eq_defaults(void);
