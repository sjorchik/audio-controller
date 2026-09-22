#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    UI_PAGE_HOME = 0,
    UI_PAGE_SOURCE,
    UI_PAGE_VOLUME,
    UI_PAGE_EQ,
    UI_PAGE_STANDBY,
    UI_PAGE_ERROR,
    UI_PAGE_MAX
} ui_page_t;

esp_err_t ui_init(void);
esp_err_t ui_show_page(ui_page_t page);
esp_err_t ui_set_backlight_percent(uint32_t percent);
