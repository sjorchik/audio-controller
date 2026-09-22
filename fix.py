import os

print("Фіксую файли проєкту...")

# 1. components/audio/CMakeLists.txt (Додаємо REQUIRES power)
with open("components/audio/CMakeLists.txt", "w", encoding="utf-8") as f:
    f.write("""idf_component_register(SRCS "audio.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bsp power freertos esp_driver_i2s esp_pm)
""")

# 2. components/audio/include/audio.h (Прибираємо помилкову 'S' та фіксуємо include)
with open("components/audio/include/audio.h", "w", encoding="utf-8") as f:
    f.write("""#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "bsp.h"

typedef struct {
    uint32_t underrun;
    uint32_t overrun;
    uint32_t cpu_load_percent;
} audio_stats_t;

esp_err_t audio_init(void);
esp_err_t audio_pipeline_start(void);
esp_err_t audio_pipeline_stop(void);
esp_err_t audio_get_stats(audio_stats_t *stats);

void audio_task_entry(void *arg);

esp_err_t audio_start(void);
esp_err_t audio_stop(void);
esp_err_t audio_set_source(bsp_audio_source_t source);
esp_err_t audio_set_volume_db(int8_t volume_db);
esp_err_t audio_set_source_trim_db(bsp_audio_source_t source, int8_t trim_db);
esp_err_t audio_apply_eq_defaults(void);
""")

# 3. components/audio/audio.c (Видалено get_clk_info, виправлено mute)
with open("components/audio/audio.c", "w", encoding="utf-8") as f:
    f.write("""#include "audio.h"
#include "power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include <string.h>

static const char *TAG = "audio";

#define AUDIO_BLOCK_FRAMES    256
#define AUDIO_DMA_DESC_NUM    3
#define AUDIO_BLOCK_BYTES     (AUDIO_BLOCK_FRAMES * sizeof(int32_t) * 2)

static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static esp_pm_lock_handle_t s_pm_lock = NULL;

static volatile bool s_running = false;

static uint32_t s_stats_underrun = 0;
static uint32_t s_stats_overrun = 0;
static uint32_t s_stats_cpu_load = 0;

typedef enum {
    AUDIO_CMD_NONE = 0,
    AUDIO_CMD_START,
    AUDIO_CMD_STOP,
} audio_cmd_t;

static QueueHandle_t s_cmd_queue = NULL;

static void audio_process_block(int32_t *in_buf, int32_t *out_buf, size_t frames, float gain)
{
    if (in_buf && out_buf) {
        for (size_t i = 0; i < frames * 2; i++) {
            out_buf[i] = (int32_t)((float)in_buf[i] * gain);
        }
    }
}

esp_err_t audio_init(void)
{
    s_cmd_queue = xQueueCreate(4, sizeof(audio_cmd_t));
    if (!s_cmd_queue) return ESP_ERR_NO_MEM;
    
    esp_err_t err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "audio_pm_lock", &s_pm_lock);
    if (err != ESP_OK) return err;

    return ESP_OK;
}

esp_err_t audio_pipeline_start(void) {
    if (s_running) return ESP_ERR_INVALID_STATE;
    audio_cmd_t cmd = AUDIO_CMD_START;
    return (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdPASS) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_pipeline_stop(void) {
    if (!s_running) return ESP_ERR_INVALID_STATE;
    audio_cmd_t cmd = AUDIO_CMD_STOP;
    return (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdPASS) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_get_stats(audio_stats_t *stats) {
    if (!stats) return ESP_ERR_INVALID_ARG;
    stats->underrun = s_stats_underrun;
    stats->overrun = s_stats_overrun;
    stats->cpu_load_percent = s_stats_cpu_load;
    return ESP_OK;
}

static void i2s_setup(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0, .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_DMA_DESC_NUM, .dma_frame_num = AUDIO_BLOCK_FRAMES,
        .auto_clear = true, .auto_clear_pre_write = true, .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan));

    i2s_std_clk_config_t clk_cfg = {
        .sample_rate_hz = BSP_AUDIO_SAMPLE_RATE_HZ,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    };

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BSP_AUDIO_SLOT_BIT_WIDTH, I2S_SLOT_MODE_STEREO);
    slot_cfg.data_bit_width = BSP_AUDIO_DATA_BIT_WIDTH;
    slot_cfg.bit_shift = true;
    slot_cfg.left_align = true;

    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = BSP_PIN_I2S_MCLK, .bclk = BSP_PIN_I2S_BCLK, .ws = BSP_PIN_I2S_WS,
            .dout = BSP_PIN_I2S_DOUT, .din = BSP_PIN_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));

    ESP_LOGI(TAG, "Такти (розрахунок): MCLK=%lu Hz, BCLK=%lu Hz, WS=%lu Hz", 
             (unsigned long)(BSP_AUDIO_SAMPLE_RATE_HZ * BSP_AUDIO_MCLK_FS),
             (unsigned long)(BSP_AUDIO_SAMPLE_RATE_HZ * BSP_AUDIO_BCLK_FS),
             (unsigned long)BSP_AUDIO_SAMPLE_RATE_HZ);
}

static void i2s_teardown(void)
{
    if (s_tx_chan) { i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    if (s_rx_chan) { i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }
}

void audio_task_entry(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "audio task started, core=%d", (int)xPortGetCoreID());

    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    
    int32_t *rx_buf = (int32_t *)heap_caps_malloc(AUDIO_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int32_t *tx_buf = (int32_t *)heap_caps_malloc(AUDIO_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    
    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "Виділено rx/tx буфери. Internal DMA heap до: %u, після: %u (дельта: %d)",
             (unsigned int)free_before, (unsigned int)free_after, (int)(free_before - free_after));

    if (!rx_buf || !tx_buf) { ESP_LOGE(TAG, "Не вдалося виділити аудіо буфери"); vTaskDelete(NULL); return; }

    while (1) {
        audio_cmd_t cmd = AUDIO_CMD_NONE;
        if (xQueueReceive(s_cmd_queue, &cmd, s_running ? pdMS_TO_TICKS(100) : portMAX_DELAY) == pdPASS) {
            if (cmd == AUDIO_CMD_START && !s_running) {
                ESP_LOGI(TAG, "Запуск аудіо пайплайну");
                esp_pm_lock_acquire(s_pm_lock);
                i2s_setup();
                
                ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
                ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
                s_running = true;

                memset(tx_buf, 0, AUDIO_BLOCK_BYTES);
                size_t bytes_written = 0;
                for (int i = 0; i < 2; i++) {
                    size_t bytes_read = 0;
                    i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES, &bytes_read, portMAX_DELAY);
                    i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES, &bytes_written, portMAX_DELAY);
                }
                
                for (int b = 0; b < 2; b++) {
                    size_t bytes_read = 0;
                    i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES, &bytes_read, portMAX_DELAY);
                    float gain = (float)(b + 1) / 2.0f;
                    audio_process_block(rx_buf, tx_buf, AUDIO_BLOCK_FRAMES, gain);
                    i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES, &bytes_written, portMAX_DELAY);
                }
                
                power_audio_pipeline_ready();
            } 
            else if (cmd == AUDIO_CMD_STOP && s_running) {
                ESP_LOGI(TAG, "Зупинка аудіо пайплайну");
                
                for (int b = 0; b < 2; b++) {
                    size_t bytes_read = 0, bytes_written = 0;
                    i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES, &bytes_read, portMAX_DELAY);
                    float gain = 1.0f - ((float)(b + 1) / 2.0f);
                    audio_process_block(rx_buf, tx_buf, AUDIO_BLOCK_FRAMES, gain);
                    i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES, &bytes_written, portMAX_DELAY);
                }
                
                memset(tx_buf, 0, AUDIO_BLOCK_BYTES);
                size_t bytes_written = 0;
                i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES, &bytes_written, portMAX_DELAY);
                
                i2s_channel_disable(s_rx_chan);
                i2s_channel_disable(s_tx_chan);
                i2s_teardown();
                esp_pm_lock_release(s_pm_lock);
                s_running = false;
                
                power_audio_pipeline_stopped();
            }
        }

        if (s_running) {
            size_t bytes_read = 0, bytes_written = 0;
            
            i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES, &bytes_read, portMAX_DELAY);
            audio_process_block(rx_buf, tx_buf, AUDIO_BLOCK_FRAMES, 1.0f);
            i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES, &bytes_written, portMAX_DELAY);
            
            if (bytes_read < AUDIO_BLOCK_BYTES) s_stats_overrun++;
            if (bytes_written < AUDIO_BLOCK_BYTES) s_stats_underrun++;
            
            s_stats_cpu_load = 5; 
        }
    }
}

esp_err_t audio_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_set_source(bsp_audio_source_t source) { (void)source; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_set_volume_db(int8_t volume_db) { (void)volume_db; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_set_source_trim_db(bsp_audio_source_t source, int8_t trim_db) { (void)source; (void)trim_db; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_apply_eq_defaults(void) { return ESP_ERR_NOT_SUPPORTED; }
""")

# 4. components/power/CMakeLists.txt
with open("components/power/CMakeLists.txt", "w", encoding="utf-8") as f:
    f.write("""idf_component_register(SRCS "power.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bsp esp_driver_gpio)
""")

# 5. components/power/include/power.h
with open("components/power/include/power.h", "w", encoding="utf-8") as f:
    f.write("""#pragma once

#include "esp_err.h"

typedef enum {
    POWER_STATE_BOOT = 0,
    POWER_STATE_RUN,
    POWER_STATE_STANDBY,
    POWER_STATE_ERROR
} power_state_t;

esp_err_t power_init(void);
esp_err_t power_set_state(power_state_t state);
power_state_t power_get_state(void);

esp_err_t power_audio_pipeline_ready(void);
esp_err_t power_audio_pipeline_stopped(void);
""")

# 6. components/power/power.c
with open("components/power/power.c", "w", encoding="utf-8") as f:
    f.write("""#include "power.h"
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
""")

# 7. Додаємо #include "esp_err.h" до всіх інших заголовків у components/, якщо його немає
for root, dirs, files in os.walk("components"):
    for file in files:
        if file.endswith(".h"):
            filepath = os.path.join(root, file)
            with open(filepath, "r", encoding="utf-8") as f:
                content = f.read()
            if 'esp_err.h' not in content:
                with open(filepath, "w", encoding="utf-8") as f:
                    f.write('#include "esp_err.h"\n' + content)

print("Файли успішно оновлено! Запустіть 'pio run' ще раз.")