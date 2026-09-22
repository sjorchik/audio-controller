#include "bsp.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "bsp";

esp_err_t bsp_init(void)
{
    // Специфікація v2.2: до цієї точки лінію XSMT тримає Low зовнішній pulldown
    // 10 kOhm (ESP32 у reset / high-Z). Тут ми перехоплюємо керування і
    // НАДАЛІ ТРИМАЄМО MUTE, поки power не повідомить про готовність аудіо.
    const gpio_config_t xsmt_cfg = {
        .pin_bit_mask = (1ULL << BSP_PIN_PCM5102_XSMT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // зовнішній pulldown уже на платі
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&xsmt_cfg);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(BSP_PIN_PCM5102_XSMT, 0); // mute до готовності пайплайну

    ESP_LOGI(TAG, "bsp_init: stub, XSMT mute held");
    return ESP_OK;
}

esp_err_t bsp_audio_set_dac_mute(bool mute)
{
    return gpio_set_level(BSP_PIN_PCM5102_XSMT, mute ? 0 : 1);
}

esp_err_t bsp_deinit(void)
{
    return ESP_OK;
}