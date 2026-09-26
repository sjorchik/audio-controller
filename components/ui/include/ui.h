// Файл: components/ui/include/ui.h
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Ініціалізація bring-up: SPI ST7789, підсвітка LEDC, LVGL, екрани.
 * Викликається один раз з app_main ДО створення задачі ui. */
esp_err_t ui_init(void);

/* Тіло задачі ui (core 0, пріоритет 8): tick-очікування подій шини system,
 * event-driven рендер + fallback 100 мс, lv_timer_handler(). */
void ui_task_entry(void *arg);

/* Dev-команда bring-up: тест-патерни для перевірки offsets/кольорів.
 * Реєструється консоллю (system.c) через extern-оголошення. */
int ui_cmd_lcdtest(int argc, char **argv);