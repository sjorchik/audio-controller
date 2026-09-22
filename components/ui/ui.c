#include "ui.h"
#include "esp_log.h"

static const char *TAG = "ui";

esp_err_t ui_init(void)
{
    ESP_LOGI(TAG, "ui_init: stub");
    return ESP_OK;
}

esp_err_t ui_show_page(ui_page_t page)
{
    (void)page;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ui_set_backlight_percent(uint32_t percent)
{
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
}
