#include "power.h"
#include "bsp.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "power";
static power_state_t s_state = POWER_STATE_BOOT;

esp_err_t power_init(void)
{
    s_state = POWER_STATE_BOOT;

    // Перевірка (специфікація v2.2): XSMT має бути Low від моменту подачі
    // живлення (зовнішній pulldown) і до unmute після готовності аудіопайплайну.
    // На цьому етапі bsp уже тримає Low програмно.
    if (gpio_get_level(BSP_PIN_PCM5102_XSMT) != 0) {
        ESP_LOGE(TAG, "XSMT is not Low at BOOT: check 10k pulldown and board conflicts");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "power_init: stub, XSMT mute verified");
    return ESP_OK;
}

esp_err_t power_set_state(power_state_t state)
{
    ESP_LOGI(TAG, "power_set_state: %d -> %d", (int)s_state, (int)state);
    s_state = state;
    return ESP_OK;
}

power_state_t power_get_state(void)
{
    return s_state;
}

esp_err_t power_audio_pipeline_ready(void)
{
    // TODO(audio-A): викликати після того, як audio_start() підтвердить
    // готовність пайплайну. Тільки тут відбувається unmute:
    //   return bsp_audio_set_dac_mute(false);
    return ESP_ERR_NOT_SUPPORTED;
}