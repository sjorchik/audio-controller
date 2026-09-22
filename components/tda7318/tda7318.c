#include "tda7318.h"
#include "esp_log.h"

static const char *TAG = "tda7318";

esp_err_t tda7318_init(void)
{
    ESP_LOGI(TAG, "tda7318_init: stub");
    return ESP_OK;
}

esp_err_t tda7318_select_source(bsp_audio_source_t source)
{
    (void)source;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tda7318_set_mute(bool mute)
{
    (void)mute;
    return ESP_ERR_NOT_SUPPORTED;
}
