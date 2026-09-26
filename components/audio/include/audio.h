// Файл: components/audio/include/audio.h
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "bsp.h"

typedef struct { uint32_t underrun; uint32_t overrun; uint32_t cpu_load_percent; } audio_stats_t;

esp_err_t audio_init(void);
esp_err_t audio_pipeline_start(void);
esp_err_t audio_pipeline_stop(void);
esp_err_t audio_get_stats(audio_stats_t *stats);
void      audio_task_entry(void *arg);

void  audio_set_volume_db(float db);
float audio_get_volume_db(void);

void  audio_set_mute(bool mute);
bool  audio_get_mute(void);

void  audio_set_eq_band(uint8_t idx, float db);
void  audio_get_eq(float out[10]);

typedef enum {
    AUDIO_EQ_PRESET_FLAT = 0, AUDIO_EQ_PRESET_ROCK, AUDIO_EQ_PRESET_POP, AUDIO_EQ_PRESET_JAZZ,
    AUDIO_EQ_PRESET_CLASSIC, AUDIO_EQ_PRESET_VOCAL, AUDIO_EQ_PRESET_NIGHT, AUDIO_EQ_PRESET_MAX
} audio_eq_preset_t;

void              audio_set_preset(audio_eq_preset_t preset);
audio_eq_preset_t audio_get_preset(void);

void  audio_set_source(bsp_audio_source_t source);

void  audio_set_source_trim_db(bsp_audio_source_t source, float db);
float audio_get_source_trim_db(bsp_audio_source_t source);