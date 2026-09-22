#include "bsp.h"
#include "esp_log.h"

static const char *TAG = "bsp";

esp_err_t bsp_init(void)
{
    ESP_LOGI(TAG, "bsp_init: stub");
    return ESP_OK;
}

esp_err_t bsp_deinit(void)
{
    return ESP_OK;
}
