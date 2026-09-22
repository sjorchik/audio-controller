#include "audio.h"
#include "esp_log.h"

static const char *TAG = "audio";

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG, "audio_init: stub");
    return ESP_OK;
}

esp_err_t audio_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_set_source(bsp_audio_source_t source)
{
    (void)source;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_set_volume_db(int8_t volume_db)
{
    (void)volume_db;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_set_source_trim_db(bsp_audio_source_t source, int8_t trim_db)
{
    (void)source;
    (void)trim_db;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_apply_eq_defaults(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
