/*
 * audio_process.h — ВНУТРІШНІЙ інтерфейс DSP-модуля.
 *
 * Використовується ТІЛЬКИ всередині компонента audio (audio.c).
 * Публічне API (volume, EQ, preset, trim) оголошене в audio.h.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define DSP_BLOCK_FRAMES  256
#define DSP_NUM_EQ_BANDS  10

/* Ініціалізація DSP-ланцюжка (коефіцієнти біквадів, DC-blocker, стан
 * лімітера, ramp). Викликається один раз в audio_init(). */
void audio_process_init(float fs);

/* Обробка одного DMA-блоку (256 стерео-фреймів, 32-bit MSB-aligned).
 * Викликається з hot-path audio_task_entry().
 * Порядок: DC-blocker → per-source trim → 10-band EQ → volume ramp → limiter.
 *
 * Жодних динамічних алокацій. Коефіцієнти біквадів оновлюються ТІЛЬКИ
 * через API-функції з audio.h (подвійне буферування).
 */
void audio_process_block(int32_t *in_buf, int32_t *out_buf, uint32_t frames);