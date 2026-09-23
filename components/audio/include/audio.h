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

/* ── Життєвий цикл пайплайну (транспорт, без змін) ── */
esp_err_t audio_init(void);
esp_err_t audio_pipeline_start(void);
esp_err_t audio_pipeline_stop(void);
esp_err_t audio_get_stats(audio_stats_t *stats);
void      audio_task_entry(void *arg);

/* ── DSP-керування (audio-B, пункт 2 ROADMAP) ── */
/* Гучність: -90..0 dB, ramp 30 мс лінійний у dB-домені */
void  audio_set_volume_db(float db);
float audio_get_volume_db(void);

/* Mute: ramp до -90 dB і назад до попереднього значення */
void  audio_set_mute(bool mute);

/* 10-смуговий графічний EQ, peaking biquad, Q=1.41, ±12 dB */
void  audio_set_eq_band(uint8_t idx, float db);
void  audio_get_eq(float out[10]);

/* Пресети EQ (фіксовані специфікацією v2.2.2) */
typedef enum {
    AUDIO_EQ_PRESET_FLAT = 0,
    AUDIO_EQ_PRESET_ROCK,
    AUDIO_EQ_PRESET_POP,
    AUDIO_EQ_PRESET_JAZZ,
    AUDIO_EQ_PRESET_CLASSIC,
    AUDIO_EQ_PRESET_VOCAL,
    AUDIO_EQ_PRESET_NIGHT,
    AUDIO_EQ_PRESET_MAX
} audio_eq_preset_t;

void              audio_set_preset(audio_eq_preset_t preset);
audio_eq_preset_t audio_get_preset(void);

/* Активне джерело (для per-source trim) */
void  audio_set_source(bsp_audio_source_t source);

/* Per-source trim: -12..+6 dB */
void  audio_set_source_trim_db(bsp_audio_source_t source, float db);
float audio_get_source_trim_db(bsp_audio_source_t source);