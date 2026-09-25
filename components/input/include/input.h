#pragma once

#include "esp_err.h"
#include "system.h" // Для типів input_source_t, input_action_t, input_event_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Ініціалізація підсистеми вводу (GPIO, PCNT).
 */
esp_err_t input_init(void);

/**
 * @brief Основний цикл задачі input (викликається з app_main).
 */
void input_task_entry(void *arg);

#ifdef __cplusplus
}
#endif