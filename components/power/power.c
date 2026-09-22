#include "power.h"
#include "bsp.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "power";
static power_state_t s_state = POWER_STATE_BOOT;

esp_err_t power_init(void)
{
    s_state = POWER_STATE_BOOT;
    if (gpio_get_level(BSP_PIN_PCM5102_XSMT) != 0) {
        ESP_LOGE(TAG, "XSMT is not Low at BOOT: check 10k pulldown and board conflicts");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "power_init: stub, XSMT mute verified");
    return ESP_OK;
}

esp_err_t power_set_state(power_state_t state) {
    s_state = state;
    return ESP_OK;
}

power_state_t power_get_state(void) { return s_state; }

esp_err_t power_audio_pipeline_ready(void) { return bsp_audio_set_dac_mute(false); }
esp_err_t power_audio_pipeline_stopped(void) { return bsp_audio_set_dac_mute(true); }
