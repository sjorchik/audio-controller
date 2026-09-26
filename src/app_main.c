// Файл: src/app_main.c
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
    if (psram_size > 0) ESP_LOGI(TAG, "PSRAM size: %u MB", (unsigned int)(psram_size / (1024 * 1024)));
    ESP_LOGI(TAG, "Free heap SPIRAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
    ESP_LOGI(TAG, "Free heap internal: %u bytes", (unsigned int)esp_get_free_internal_heap_size());
    ESP_LOGI(TAG, "Free heap total: %u bytes", (unsigned int)esp_get_free_heap_size());
}

static void audio_task(void *arg) { (void)arg; audio_task_entry(arg); }

static void ctrl_task(void *arg)
{
    (void)arg; ESP_LOGI(TAG, "ctrl task started, core=%d", (int)xPortGetCoreID());
    TickType_t last_wake_tick = xTaskGetTickCount(); uint32_t heartbeat = 0;
    while (1) {
        vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(5000)); heartbeat++;
        audio_stats_t st; audio_get_stats(&st);
        ESP_LOGI(TAG, "heartbeat=%lu state=%d heap=%u underrun=%lu overrun=%lu cpu=%lu%%",
                 (unsigned long)heartbeat, (int)power_get_state(), (unsigned int)esp_get_free_heap_size(),
                 (unsigned long)st.underrun, (unsigned long)st.overrun, (unsigned long)st.cpu_load_percent);
    }
}

static void input_task(void *arg) { (void)arg; input_task_entry(arg); }

static void ui_task(void *arg)
{
    (void)arg; ESP_LOGI(TAG, "ui task started, core=%d", (int)xPortGetCoreID());
    ui_task_entry(arg);
}

static void web_task(void *arg)
{
    (void)arg; ESP_LOGI(TAG, "web task started, core=%d", (int)xPortGetCoreID());
    while (1) { vTaskDelay(pdMS_TO_TICKS(100)); }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting audio controller skeleton");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bsp_init());
    ESP_ERROR_CHECK(system_init());
    ESP_ERROR_CHECK(settings_init());
    settings_t settings;
    ESP_ERROR_CHECK(settings_load(&settings));

    ESP_ERROR_CHECK(power_init());
    ESP_ERROR_CHECK(audio_init());
    esp_err_t tda_err = tda7318_init();
    if (tda_err != ESP_OK) ESP_LOGE(TAG, "TDA7318 init failed: %s", esp_err_to_name(tda_err));

    ESP_ERROR_CHECK(input_init());
    ESP_ERROR_CHECK(ir_init());
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(web_init());

    log_system_info();

    bool tasks_ok = true;
    tasks_ok = tasks_ok && (xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 20, NULL, 1) == pdPASS);
    tasks_ok = tasks_ok && (xTaskCreatePinnedToCore(ctrl_task,  "ctrl",  4096, NULL, 10, NULL, 0) == pdPASS);
    /* input: 8192 (запас після введення шини підписників у пункті 5 ui).
     * Реальний watermark за діагностикою bring-up: 908 Б / 8192;
     * 4096 Б було б достатньо, але 8192 дає простір для майбутніх змін input. */
    tasks_ok = tasks_ok && (xTaskCreatePinnedToCore(input_task, "input", 8192, NULL, 10, NULL, 0) == pdPASS);
    tasks_ok = tasks_ok && (xTaskCreatePinnedToCore(ui_task,    "ui",    16384, NULL, 8,  NULL, 0) == pdPASS);
    tasks_ok = tasks_ok && (xTaskCreatePinnedToCore(web_task,   "web",   6144, NULL, 5,  NULL, 0) == pdPASS);

    if (!tasks_ok) ESP_LOGE(TAG, "Failed to create one or more tasks");

    ESP_ERROR_CHECK(power_set_state(POWER_STATE_RUN));
    ESP_ERROR_CHECK(audio_pipeline_start());
}