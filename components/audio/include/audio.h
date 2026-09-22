#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "bsp.h"

typedef struct {
    uint32_t underrun;
    uint32_t overrun;
    uint32_t cpu_load_percent;
} audio_stats_t;

esp_err_t audio_init(void);
esp_err_t audio_pipeline_start(void);
esp_err_t audio_pipeline_stop(void);
esp_err_t audio_get_stats(audio_stats_t *stats);

void audio_task_entry(void *arg);

esp_err_t audio_start(void);
esp_err_t audio_stop(void);
esp_err_t audio_set_source(bsp_audio_source_t source);
esp_err_t audio_set_volume_db(int8_t volume_db);
esp_err_t audio_set_source_trim_db(bsp_audio_source_t source, int8_t trim_db);
esp_err_t audio_apply_eq_defaults(void);
