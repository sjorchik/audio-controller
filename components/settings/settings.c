#include "settings.h"
#include "esp_log.h"

static const char *TAG = "settings";

esp_err_t settings_init(void)
{
    ESP_LOGI(TAG, "settings_init: stub");
    return ESP_OK;
}

esp_err_t settings_load(settings_t *out_settings)
{
    if (out_settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    out_settings->source = BSP_AUDIO_SOURCE_TV_BOX;
    out_settings->volume_db = 0;
    out_settings->display_backlight_on = true;

    for (int i = 0; i < BSP_AUDIO_SOURCE_MAX; ++i) {
        out_settings->source_trim_db[i] = 0;
    }

    ESP_LOGI(TAG, "settings_load: defaults loaded");
    return ESP_OK;
}

esp_err_t settings_save(const settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
