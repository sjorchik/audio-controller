/*
 * dsp_presets.h — спільні константи EQ (audio_process.c та host-тести).
 * Специфікація v2.2.2, фіксовані значення. Платформо-незалежний header.
 */

#pragma once

#include "dsp_math.h"   /* DSP_NUM_EQ_BANDS */

#define DSP_EQ_PRESET_COUNT  7

/* Центральні частоти 10-смугового графічного EQ (Гц) */
static const float DSP_EQ_CENTERS[DSP_NUM_EQ_BANDS] = {
    31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};

/* Таблиці пресетів (dB). Порядок: Flat, Rock, Pop, Jazz, Classic, Vocal, Night. */
static const float DSP_EQ_PRESETS[DSP_EQ_PRESET_COUNT][DSP_NUM_EQ_BANDS] = {
    /* FLAT    */ { 0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f},
    /* ROCK    */ { 5.0f,  4.0f,  3.0f,  1.0f,  0.0f, -1.0f,  0.0f,  2.0f,  3.0f,  4.0f},
    /* POP     */ {-1.0f,  0.0f,  1.0f,  2.0f,  3.0f,  3.0f,  2.0f,  1.0f,  0.0f, -1.0f},
    /* JAZZ    */ { 3.0f,  2.0f,  1.0f,  2.0f, -1.0f, -1.0f,  0.0f,  1.0f,  2.0f,  3.0f},
    /* CLASSIC */ { 4.0f,  3.0f,  2.0f,  1.0f,  0.0f,  0.0f, -1.0f,  0.0f,  2.0f,  3.0f},
    /* VOCAL   */ {-2.0f, -1.0f,  0.0f,  2.0f,  4.0f,  4.0f,  3.0f,  2.0f,  0.0f, -1.0f},
    /* NIGHT   */ { 3.0f,  2.0f,  1.0f,  0.0f,  0.0f,  0.0f,  0.0f, -1.0f, -2.0f, -3.0f},
};