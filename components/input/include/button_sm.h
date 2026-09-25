#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Дії, які генерує стан-машина кнопки */
typedef enum {
    BTN_ACTION_NONE = 0,
    BTN_ACTION_SHORT,
    BTN_ACTION_LONG,
    BTN_ACTION_REPEAT,
} btn_action_t;

/* Конфігурація стан-машини (для різних типів кнопок) */
typedef struct {
    bool repeat_allowed; // true для UP/DOWN/LEFT/RIGHT, false для POWER/OK
} button_sm_config_t;

/* Стан однієї кнопки */
typedef struct {
    uint8_t state;
    uint32_t event_time_ms;  // час останнього переходу між станами
    uint32_t press_time_ms;  // час натискання (для обчислення довжини утримання)
    uint32_t last_repeat_ms; // час останнього REPEAT
    bool long_fired;         // LONG вже згенеровано для цього натискання
    bool short_pending;      // SHORT зафіксовано, але відкладено до стабільного
                             // release-дебанусу (захист від бренькоту на відпусканні)
    button_sm_config_t cfg;
} button_sm_t;

/**
 * @brief Ініціалізація стан-машини кнопки.
 * @param sm Вказівник на структуру стан-машини.
 * @param repeat_allowed Дозволити генерацію REPEAT (для стрілок).
 */
void button_sm_init(button_sm_t *sm, bool repeat_allowed);

/**
 * @brief Обробка поточного рівня кнопки.
 * @param sm Вказівник на структуру стан-машини.
 * @param level_active true, якщо кнопка натиснута (активний рівень).
 * @param now_ms Поточний час у мілісекундах (монотонний).
 * @return Дію, яку потрібно виконати (BTN_ACTION_NONE, якщо події немає).
 *
 * @note НЕ потокобезпечна: викликати тільки з однієї задачі (input_task)
 *       з монотонним зростанням now_ms.
 * @note Семантика подій:
 *       - SHORT видається ПІСЛЯ стабільного release-дебанусу (рівно 1 подія
 *         на натискання навіть при бренькоті контактів на відпусканні);
 *       - LONG — одноразово на LONG_THRESHOLD_MS утримання;
 *       - REPEAT — кожні REPEAT_INTERVAL_MS після REPEAT_THRESHOLD_MS
 *         (лише якщо repeat_allowed).
 */
btn_action_t button_sm_update(button_sm_t *sm, bool level_active, uint32_t now_ms);

#ifdef __cplusplus
}
#endif