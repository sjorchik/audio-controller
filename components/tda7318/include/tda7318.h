#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "bsp.h"
#include "driver/i2c_master.h"

// 7-бітна адреса з даташиту: фіксована для TDA7318 та PT2313/TDA7313
#define TDA7318_I2C_ADDR 0x44

typedef struct {
    uint32_t i2c_errors;  // Лічильник помилок I2C-транзакцій
} tda7318_stats_t;

/**
 * @brief Ініціалізація I2C-шини та селектора входів.
 * Фіксує згідно специфікації v2: volume 0 dB, tone flat, loudness off;
 * стан після init = mute. Помилки I2C: лічильник + повернення esp_err (без abort).
 */
esp_err_t tda7318_init(void);

/**
 * @brief Перемикає вхід селектора (стан mute зберігається).
 * Мапа: TV_BOX=IN0, COMPUTER=IN1, BLUETOOTH=IN2, AUX=IN3.
 */
esp_err_t tda7318_select_source(bsp_audio_source_t source);

/** @brief Soft Mute / unmute самого селектора (для STANDBY у пункті 8). */
esp_err_t tda7318_set_mute(bool mute);

/**
 * @brief ПОВНА безклікова послідовність: audio_set_mute(true) -> 35 мс ->
 * select_source -> 30 мс (аналогове заспокоєння) -> audio_set_mute(false).
 * Контекст: задача (ctrl/REPL), не ISR.
 */
esp_err_t tda7318_switch_source(bsp_audio_source_t source);

/** @brief Статистика помилок I2C. */
void tda7318_get_stats(tda7318_stats_t *stats);

/** @brief Хендл I2C-шини для dev-команди i2cscan. */
i2c_master_bus_handle_t tda7318_get_i2c_bus(void);

/**
 * @brief Bring-up діагностика: прямий запис байта команди у селектор.
 * Використовується dev-командою 'raw' для емпіричної перевірки кодів
 * (полярність volume/mute/gain) без перепрошивок.
 */
esp_err_t tda7318_raw_write(uint8_t cmd);