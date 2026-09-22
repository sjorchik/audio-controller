#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "bsp.h"
#include "system.h"
#include "settings.h"
#include "power.h"
#include "audio.h"
#include "tda7318.h"
#include "input.h"
#include "ir.h"
#include "ui.h"
#include "web.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

static const char *TAG = "app_main";

static void log_system_info(void)
{
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "CPU cores: %d", (int)CONFIG_FREERTOS_NUMBER_OF_CORES);
    ESP_LOGI(TAG, "Flash size: %lu MB", (unsigned long)(flash_size / (1024 * 1024)));

#if CONFIG_SPIRAM
    size_t psram_size = esp_psram_get_size();
    if (psram_size > 0) {
        ESP_LOGI(TAG, "PSRAM size: %u MB", (unsigned int)(psram_size / (1024 * 1024)));
    } else {
        ESP_LOGW(TAG, "PSRAM not detected");
    }
    ESP_LOGI(TAG, "Free heap SPIRAM: %u bytes",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif

    ESP_LOGI(TAG, "Free heap internal: %u bytes",
             (unsigned int)esp_get_free_internal_heap_size());
    ESP_LOGI(TAG, "Free heap total: %u bytes",
             (unsigned int)esp_get_free_heap_size());
}

static void audio_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "audio task started, core=%d", (int)xPortGetCoreID());

    // TODO(audio): I2S RX/TX, DC-blocker, trim, EQ, volume ramp, limiter.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void ctrl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ctrl task started, core=%d", (int)xPortGetCoreID());

    TickType_t last_wake_tick = xTaskGetTickCount();
    uint32_t heartbeat = 0;

    while (1) {
        vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(5000));
        heartbeat++;
        ESP_LOGI(TAG, "heartbeat=%lu state=%d free_heap=%u",
                 (unsigned long)heartbeat,
                 (int)power_get_state(),
                 (unsigned int)esp_get_free_heap_size());
    }
}

static void input_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "input task started, core=%d", (int)xPortGetCoreID());

    // TODO(input): кнопки, енкодер (PCNT), антидребезг, події у system.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ui task started, core=%d", (int)xPortGetCoreID());

    // TODO(ui): ST7789 SPI, підсвітка LEDC, LVGL.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void web_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "web task started, core=%d", (int)xPortGetCoreID());

    // TODO(web): Wi-Fi STA/AP, HTTP REST, WebSocket. У скелеті нічого не стартує.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting audio controller skeleton");

    // 1. NVS: налаштування, калібрування Wi-Fi у майбутньому.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 2. Апаратна платформа.
    // Специфікація v2.2: відтепер XSMT тримає Low bsp; до цього моменту лінію
    // тримав зовнішній pulldown 10 kOhm. Unmute (XSMT High) виконає ТІЛЬКИ power
    // через power_audio_pipeline_ready() після готовності аудіопайплайну.
    // У скелеті ЦАП навмисно лишається в mute.
    ESP_ERROR_CHECK(bsp_init());

    // 3. Шина подій.
    ESP_ERROR_CHECK(system_init());

    // 4. Налаштування.
    ESP_ERROR_CHECK(settings_init());
    settings_t settings;
    ESP_ERROR_CHECK(settings_load(&settings));
    ESP_LOGI(TAG, "Settings loaded: source=%d volume_db=%d backlight=%d",
             (int)settings.source,
             (int)settings.volume_db,
             (int)settings.display_backlight_on);

    // 5. Компоненти.
    ESP_ERROR_CHECK(power_init());
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(tda7318_init());
    ESP_ERROR_CHECK(input_init());
    ESP_ERROR_CHECK(ir_init());
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(web_init());

    // 6. Інформація про систему.
    log_system_info();

    // 7. Задачі.
    // audio -> core 1, високий пріоритет: ізольовано від Wi-Fi/UI на core 0.
    // ctrl/input/ui/web -> core 0.
    bool tasks_ok = true;

    tasks_ok = tasks_ok && (xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 20, NULL, 1) == pdPASS);
    tasks_ok = tasks_ok && (xTaskCreatePinnedToCore(ctrl_task,  "ctrl",  4096, NULL, 10, NULL, 0) == pdPASS);
    tasks_ok = tasks_ok && (xTaskCreatePinnedToCore(input_task, "input", 4096, NULL, 10, NULL, 0) == pdPASS);
    tasks_ok = tasks_ok && (xTaskCreatePinnedToCore(ui_task,    "ui",    8192, NULL, 8,  NULL, 0) == pdPASS);
    tasks_ok = tasks_ok && (xTaskCreatePinnedToCore(web_task,   "web",   6144, NULL, 5,  NULL, 0) == pdPASS);

    if (!tasks_ok) {
        ESP_LOGE(TAG, "Failed to create one or more tasks");
    }

    // 8. Стан скелета. У реальній системі до готовності аудіо тримаємо BOOT/mute.
    ESP_ERROR_CHECK(power_set_state(POWER_STATE_RUN));

    // TODO(power): інтеграція з esp_pm.
    // RUN: аудіозадача тримає esp_pm_lock (max freq), STANDBY: lock звільняється,
    // частота падає, Wi-Fi/display/audio вимкнені, XSMT mute, TDA mute.
    // Приклад майбутнього коду:
    // esp_pm_config_t pm_cfg = { .max_freq_mhz = 240, .min_freq_mhz = 20, .light_sleep_enable = false };
    // esp_pm_configure(&pm_cfg);
}