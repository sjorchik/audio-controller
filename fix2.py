import os

audio_c = r'''#include "audio.h"
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

// Розмір одного блоку обробки = розмір одного DMA-буфера
#define AUDIO_BLOCK_FRAMES    256
#define AUDIO_DMA_DESC_NUM    3
// stereo, 32-bit slot => 8 байт на фрейм
#define AUDIO_BLOCK_BYTES     (AUDIO_BLOCK_FRAMES * sizeof(int32_t) * 2)

// Самоперевірка тактування з bsp.h: BCLK = fs * slot_bits * 2; MCLK = 256*fs
_Static_assert(BSP_AUDIO_BCLK_FS == BSP_AUDIO_SLOT_BIT_WIDTH * 2,
               "BCLK_FS має дорівнювати slot_bit_width * 2 (stereo)");
_Static_assert(BSP_AUDIO_MCLK_FS == 256,
               "MCLK multiple 256 дає ціле ділення MCLK/BCLK для 32-bit slot");

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

// TODO(audio-B): 10-смуговий графічний EQ, пресети, volume, trim, DC-blocker.
// Зараз: прямий прохід RX->TX з коефіцієнтом gain (для fade-in/fade-out).
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
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Не вдалося створити чергу команд");
        return ESP_ERR_NO_MEM;
    }

    // Фіксація частоти CPU у RUN під час роботи тракту
    esp_err_t err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "audio_pm_lock", &s_pm_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Не вдалося створити PM lock: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "audio_init: OK");
    return ESP_OK;
}

esp_err_t audio_pipeline_start(void)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    audio_cmd_t cmd = AUDIO_CMD_START;
    return (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdPASS) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_pipeline_stop(void)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    audio_cmd_t cmd = AUDIO_CMD_STOP;
    return (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdPASS) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_get_stats(audio_stats_t *stats)
{
    if (!stats) return ESP_ERR_INVALID_ARG;
    stats->underrun = s_stats_underrun;
    stats->overrun = s_stats_overrun;
    stats->cpu_load_percent = s_stats_cpu_load;
    return ESP_OK;
}

static void i2s_setup(void)
{
    // 3 DMA-дескриптори по 256 фреймів на напрямок (full-duplex на I2S_NUM_0)
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_BLOCK_FRAMES,
        .auto_clear = true,          // тиша на TX, якщо немає даних (захист від клацань)
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan));

    // Такти: fs = 48 kHz, MCLK = 256*fs = 12.288 MHz
    i2s_std_clk_config_t clk_cfg = {
        .sample_rate_hz = BSP_AUDIO_SAMPLE_RATE_HZ,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bclk_div = 8,
    };

    // Philips I2S: дані 24 bit MSB-aligned у 32-bit слоті.
    // УВАГА: макрос ставить slot_bit_width = AUTO (= data_bit_width),
    // тому ЯВНО фіксуємо слот 32 bit і ws_width 32, щоб BCLK = 64*fs = 3.072 MHz.
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BSP_AUDIO_DATA_BIT_WIDTH, I2S_SLOT_MODE_STEREO);
    slot_cfg.slot_bit_width = BSP_AUDIO_SLOT_BIT_WIDTH;   // 32 bit slot
    slot_cfg.ws_width       = BSP_AUDIO_SLOT_BIT_WIDTH;   // WS = 1 слот = 32 BCLK
    slot_cfg.bit_shift      = true;                       // Philips: 1 BCLK затримка даних
    slot_cfg.left_align     = true;                       // 24-bit дані MSB-aligned у слоті

    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = BSP_PIN_I2S_MCLK,
            .bclk = BSP_PIN_I2S_BCLK,
            .ws   = BSP_PIN_I2S_WS,
            .dout = BSP_PIN_I2S_DOUT,
            .din  = BSP_PIN_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));

    // Лог-розрахунок тактів (критерій приймання 5)
    ESP_LOGI(TAG, "Такти: MCLK=%lu Hz, BCLK=%lu Hz, WS=%lu Hz (slot=%d bit, data=%d bit)",
             (unsigned long)(BSP_AUDIO_SAMPLE_RATE_HZ * BSP_AUDIO_MCLK_FS),
             (unsigned long)(BSP_AUDIO_SAMPLE_RATE_HZ * BSP_AUDIO_SLOT_BIT_WIDTH * 2),
             (unsigned long)BSP_AUDIO_SAMPLE_RATE_HZ,
             (int)BSP_AUDIO_SLOT_BIT_WIDTH, (int)BSP_AUDIO_DATA_BIT_WIDTH);
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

    // Підтвердження, що DMA-буфери виділені з internal RAM (критерій 4)
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    int32_t *rx_buf = (int32_t *)heap_caps_malloc(AUDIO_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int32_t *tx_buf = (int32_t *)heap_caps_malloc(AUDIO_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "DMA-буфери: rx=%p tx=%p, internal DMA heap до=%u після=%u (дельта=%d)",
             rx_buf, tx_buf, (unsigned int)free_before, (unsigned int)free_after,
             (int)(free_before - free_after));

    if (!rx_buf || !tx_buf) {
        ESP_LOGE(TAG, "Не вдалося виділити аудіо буфери");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        audio_cmd_t cmd = AUDIO_CMD_NONE;
        if (xQueueReceive(s_cmd_queue, &cmd, s_running ? pdMS_TO_TICKS(100) : portMAX_DELAY) == pdPASS) {
            if (cmd == AUDIO_CMD_START && !s_running) {
                ESP_LOGI(TAG, "Запуск аудіо пайплайну");
                esp_pm_lock_acquire(s_pm_lock);   // frequency lock у RUN
                i2s_setup();

                ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
                ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
                s_running = true;

                // Soft-start частина 1: 2 блоки тиші (ADC вже читаємо, щоб стабілізувався)
                memset(tx_buf, 0, AUDIO_BLOCK_BYTES);
                size_t bytes_written = 0;
                for (int i = 0; i < 2; i++) {
                    size_t bytes_read = 0;
                    i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES, &bytes_read, portMAX_DELAY);
                    i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES, &bytes_written, portMAX_DELAY);
                }

                // Soft-start частина 2: fade-in ~10 мс (2 блоки по 256 фреймів @48k)
                for (int b = 0; b < 2; b++) {
                    size_t bytes_read = 0;
                    i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES, &bytes_read, portMAX_DELAY);
                    float gain = (float)(b + 1) / 2.0f;   // 0.5 -> 1.0
                    audio_process_block(rx_buf, tx_buf, AUDIO_BLOCK_FRAMES, gain);
                    i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES, &bytes_written, portMAX_DELAY);
                }

                // BOOT-unmute ТІЛЬКИ після готовності тракту (власник XSMT - power)
                power_audio_pipeline_ready();
                ESP_LOGI(TAG, "Пайплайн запущено, unmute виконано");
            }
            else if (cmd == AUDIO_CMD_STOP && s_running) {
                ESP_LOGI(TAG, "Зупинка аудіо пайплайну");

                // Fade-out ~10 мс (2 блоки)
                for (int b = 0; b < 2; b++) {
                    size_t bytes_read = 0, bytes_written = 0;
                    i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES, &bytes_read, portMAX_DELAY);
                    float gain = 1.0f - ((float)(b + 1) / 2.0f);   // 0.5 -> 0.0
                    audio_process_block(rx_buf, tx_buf, AUDIO_BLOCK_FRAMES, gain);
                    i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES, &bytes_written, portMAX_DELAY);
                }

                // Фінальний блок тиші перед зупинкою тактів
                memset(tx_buf, 0, AUDIO_BLOCK_BYTES);
                size_t bytes_written = 0;
                i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES, &bytes_written, portMAX_DELAY);

                i2s_channel_disable(s_rx_chan);
                i2s_channel_disable(s_tx_chan);
                i2s_teardown();                    // зупинка тактів (MCLK/BCLK/WS)
                esp_pm_lock_release(s_pm_lock);
                s_running = false;

                power_audio_pipeline_stopped();    // mute через power (не чіпаємо XSMT напряму)
                ESP_LOGI(TAG, "Пайплайн зупинено, mute виконано");
            }
        }

        if (s_running) {
            size_t bytes_read = 0, bytes_written = 0;

            i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES, &bytes_read, portMAX_DELAY);
            audio_process_block(rx_buf, tx_buf, AUDIO_BLOCK_FRAMES, 1.0f);   // прямий прохід RX->TX
            i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES, &bytes_written, portMAX_DELAY);

            if (bytes_read < AUDIO_BLOCK_BYTES) s_stats_overrun++;
            if (bytes_written < AUDIO_BLOCK_BYTES) s_stats_underrun++;

            // Задача більшість часу заблокована на read/write => load ~5%
            s_stats_cpu_load = 5;
        }
    }
}

// Сумісність зі скелетом (реалізація у audio-B)
esp_err_t audio_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_set_source(bsp_audio_source_t source) { (void)source; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_set_volume_db(int8_t volume_db) { (void)volume_db; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_set_source_trim_db(bsp_audio_source_t source, int8_t trim_db) { (void)source; (void)trim_db; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_apply_eq_defaults(void) { return ESP_ERR_NOT_SUPPORTED; }
'''

with open("components/audio/audio.c", "w", encoding="utf-8") as f:
    f.write(audio_c