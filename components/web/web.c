#include "web.h"
#include "esp_log.h"

static const char *TAG = "web";
static web_state_t s_state = WEB_STATE_STOPPED;

esp_err_t web_init(void)
{
    ESP_LOGI(TAG, "web_init: stub");
    s_state = WEB_STATE_STOPPED;
    return ESP_OK;
}

esp_err_t web_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t web_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

web_state_t web_get_state(void)
{
    return s_state;
}
