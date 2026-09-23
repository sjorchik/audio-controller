/*
 * audio.c — транспорт I2S + інтеграція DSP-ланцюжка (audio-B).
 *
 * Транспорт (DMA, такти, start/stop, XSMT) — БЕЗ ЗМІН відносно audio-A.
 * Єдина зміна у hot-path: замість заглушки gain-множення викликається
 * повний DSP-ланцюжок audio_process_block() (DC-blocker → trim → EQ →
 * volume ramp → лімітер).
 *
 * Soft-start / soft-stop (fade-in / fade-out) залишено як було — це
 * транспортна логіка, 2 блоки по 5.33 мс з лінійним gain.
 *
 * Специфікація v2.2.2: slot_bit_width=32, дані 24 bit MSB-aligned.
 * У робочому стані черга команд опитується неблокувально.
 * У hot-path жодних логів.
 */

#include "audio.h"
#include "audio_process.h"          /* DSP-ланцюжок (audio-B) */
#include "power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/Task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include <string.h>

static const char *TAG = "audio";

/* ── Розміри блоків (БЕЗ ЗМІН відносно audio-A) ── */
/* 256 фреймів @ 48 kHz = 5.33 мс; 3 дескриптори = ~16 мс резерву TX. */
#define AUDIO_BLOCK_FRAMES    256
#define AUDIO_DMA_DESC_NUM    3
/* stereo, 32-bit slot => 8 байт на фрейм */
#define AUDIO_BLOCK_BYTES     (AUDIO_BLOCK_FRAMES * sizeof(int32_t) * 2)

/* ── Дескриптори I2S (БЕЗ ЗМІН) ── */
static i2s_chan_handle_t    s_tx_chan  = NULL;
static i2s_chan_handle_t    s_rx_chan  = NULL;
static esp_pm_lock_handle_t s_pm_lock  = NULL;

static volatile bool s_running = false;

/* ── Статистика (читається ctrl-задачею через audio_get_stats) ── */
static uint32_t s_stats_underrun  = 0;
static uint32_t s_stats_overrun   = 0;
static uint32_t s_stats_cpu_load  = 0;
static int64_t  s_busy_us_ema     = 0;

/* ── Черга команд пайплайну (БЕЗ ЗМІН) ── */
typedef enum {
    AUDIO_CMD_NONE = 0,
    AUDIO_CMD_START,
    AUDIO_CMD_STOP,
} audio_cmd_t;

static QueueHandle_t s_cmd_queue = NULL;

/* ────────────────────────────────────────────────────────────────
 * I2S-транспорт (БЕЗ ЗМІН відносно audio-A)
 * ──────────────────────────────────────────────────────────────── */

static void i2s_setup(void)
{
    /* 3 DMA-дескриптори по 256 фреймів на напрямок (full-duplex, I2S_NUM_0) */
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0, .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_DMA_DESC_NUM, .dma_frame_num = AUDIO_BLOCK_FRAMES,
        .auto_clear = true, .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan));

    /* Такти: fs = 48 kHz, MCLK = 256*fs = 12.288 MHz */
    i2s_std_clk_config_t clk_cfg = {
        .sample_rate_hz = BSP_AUDIO_SAMPLE_RATE_HZ,
        .clk_src        = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
    };

    /*
     * Philips I2S, слот 32 bit. data_bit_width навмисно НЕ перевизначаємо
     * на 24: драйвер ESP-IDF вимагає mclk_multiple % 3 == 0 для 24 bit,
     * а специфікація v2.2 фіксує MCLK = 256*fs. 24-бітне аудіо
     * (BSP_AUDIO_DATA_BIT_WIDTH) фізично йде MSB-aligned у 32-бітному слоті.
     */
    i2s_std_slot_config_t slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BSP_AUDIO_SLOT_BIT_WIDTH,
                                            I2S_SLOT_MODE_STEREO);
    slot_cfg.bit_shift   = true;
    slot_cfg.left_align  = true;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = BSP_PIN_I2S_MCLK, .bclk = BSP_PIN_I2S_BCLK,
            .ws   = BSP_PIN_I2S_WS,   .dout = BSP_PIN_I2S_DOUT,
            .din  = BSP_PIN_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false,
                              .ws_inv = false },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));

    /* Лог-розрахунок тактів (критерій приймання 5,audio-A) */
    ESP_LOGI(TAG, "Такти: MCLK=%lu Hz, BCLK=%lu Hz, WS=%lu Hz",
             (unsigned long)(BSP_AUDIO_SAMPLE_RATE_HZ * BSP_AUDIO_MCLK_FS),
             (unsigned long)(BSP_AUDIO_SAMPLE_RATE_HZ * BSP_AUDIO_BCLK_FS),
             (unsigned long)BSP_AUDIO_SAMPLE_RATE_HZ);
}

static void i2s_teardown(void)
{
    if (s_tx_chan) { i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    if (s_rx_chan) { i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }
}

/* ────────────────────────────────────────────────────────────────
 * Ініціалізація компонента
 * ──────────────────────────────────────────────────────────────── */

esp_err_t audio_init(void)
{
    s_cmd_queue = xQueueCreate(4, sizeof(audio_cmd_t));
    if (!s_cmd_queue) return ESP_ERR_NO_MEM;

    /* Фіксація частоти CPU у RUN під час роботи тракту */
    esp_err_t err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0,
                                       "audio_pm_lock", &s_pm_lock);
    if (err != ESP_OK) return err;

    /* Ініціалізація DSP-ланцюжка (audio-B): коефіцієнти біквадів,
     * DC-blocker, стани лімітера, ramp. Один раз при старті. */
    audio_process_init((float)BSP_AUDIO_SAMPLE_RATE_HZ);

    ESP_LOGI(TAG, "DSP-ланцюжок ініціалізовано (fs=%d Hz)",
             BSP_AUDIO_SAMPLE_RATE_HZ);

    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────
 * Команди пайплайну (БЕЗ ЗМІН)
 * ──────────────────────────────────────────────────────────────── */

esp_err_t audio_pipeline_start(void)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    audio_cmd_t cmd = AUDIO_CMD_START;
    return (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdPASS)
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_pipeline_stop(void)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    audio_cmd_t cmd = AUDIO_CMD_STOP;
    return (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdPASS)
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_get_stats(audio_stats_t *stats)
{
    if (!stats) return ESP_ERR_INVALID_ARG;
    stats->underrun         = s_stats_underrun;
    stats->overrun          = s_stats_overrun;
    stats->cpu_load_percent = s_stats_cpu_load;
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────
 * Задача аудіо-тракту
 * ──────────────────────────────────────────────────────────────── */

void audio_task_entry(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "audio task started, core=%d", (int)xPortGetCoreID());

    /* DMA-буфери виключно з internal RAM (критерій: PSRAM не чіпати) */
    size_t free_before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    int32_t *rx_buf = (int32_t *)heap_caps_malloc(
        AUDIO_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int32_t *tx_buf = (int32_t *)heap_caps_malloc(
        AUDIO_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    size_t free_after =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    ESP_LOGI(TAG, "DMA-буфери: rx=%p tx=%p, heap до=%u після=%u (дельта=%d)",
             (void *)rx_buf, (void *)tx_buf,
             (unsigned)free_before, (unsigned)free_after,
             (int)(free_before - free_after));

    if (!rx_buf || !tx_buf) {
        ESP_LOGE(TAG, "Не вдалося виділити аудіо буфери");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        audio_cmd_t cmd = AUDIO_CMD_NONE;

        /*
         * КЛЮЧОВА ЛОГІКА (audio-A): поки пайплайн працює, чергу опитуємо
         * НЕБЛОКУВАЛЬНО (timeout 0) між аудіоблоками. Блокувальне
         * очікування команди лишало TX-DMA без даних -> auto_clear
         * вставляв нулі -> клацання ~10 Гц.
         * У простої (пайплайн зупинений) блокуємось повністю.
         */
        TickType_t cmd_wait = s_running ? 0 : portMAX_DELAY;

        if (xQueueReceive(s_cmd_queue, &cmd, cmd_wait) == pdPASS) {

            /* ── ЗАПУСК ── */
            if (cmd == AUDIO_CMD_START && !s_running) {
                ESP_LOGI(TAG, "Запуск аудіо пайплайну");
                esp_pm_lock_acquire(s_pm_lock);
                i2s_setup();

                ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
                ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
                s_running = true;

                /*
                 * Soft-start частина 1: 2 блоки тиші.
                 * ADC вже читаємо для стабілізації DC-blocker.
                 */
                memset(tx_buf, 0, AUDIO_BLOCK_BYTES);
                size_t bytes_written = 0;
                for (int i = 0; i < 2; i++) {
                    size_t bytes_read = 0;
                    i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES,
                                     &bytes_read, portMAX_DELAY);
                    i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES,
                                      &bytes_written, portMAX_DELAY);
                }

                /*
                 * Soft-start частина 2: fade-in ~10 мс (2 блоки по 5.33 мс).
                 * Транспортний fade-in (inline gain, БЕЗ DSP — це логіка
                 * start/stop, не змінюємо відповідно до специфікації).
                 */
                for (int b = 0; b < 2; b++) {
                    size_t bytes_read = 0;
                    i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES,
                                     &bytes_read, portMAX_DELAY);
                    float gain = (float)(b + 1) / 2.0f;  /* 0.5 → 1.0 */
                    for (size_t i = 0; i < AUDIO_BLOCK_FRAMES * 2; i++) {
                        tx_buf[i] = (int32_t)((float)rx_buf[i] * gain);
                    }
                    i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES,
                                      &bytes_written, portMAX_DELAY);
                }

                /*
                 * Встановлюємо початковий стан DSP перед робочим циклом.
                 * Гучність за замовчуванням -20 dB, mute=false.
                 */
                audio_set_mute(false);

                /* BOOT-unmute ТІЛЬКИ після готовності тракту (власник XSMT — power) */
                power_audio_pipeline_ready();
                ESP_LOGI(TAG, "Пайплайн запущено, unmute виконано");
            }

            /* ── ЗУПИНКА ── */
            else if (cmd == AUDIO_CMD_STOP && s_running) {
                ESP_LOGI(TAG, "Зупинка аудіо пайплайну");

                /*
                 * Fade-out ~10 мс (2 блоки).
                 * Транспортний fade-out (inline gain, БЕЗ DSP).
                 */
                for (int b = 0; b < 2; b++) {
                    size_t bytes_read = 0, bytes_written = 0;
                    i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES,
                                     &bytes_read, portMAX_DELAY);
                    float gain = 1.0f - ((float)(b + 1) / 2.0f); /* 0.5 → 0.0 */
                    for (size_t i = 0; i < AUDIO_BLOCK_FRAMES * 2; i++) {
                        tx_buf[i] = (int32_t)((float)rx_buf[i] * gain);
                    }
                    i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES,
                                      &bytes_written, portMAX_DELAY);
                }

                /* Фінальний блок тиші перед зупинкою тактів */
                memset(tx_buf, 0, AUDIO_BLOCK_BYTES);
                size_t bytes_written = 0;
                i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES,
                                  &bytes_written, portMAX_DELAY);

                i2s_channel_disable(s_rx_chan);
                i2s_channel_disable(s_tx_chan);
                i2s_teardown();
                esp_pm_lock_release(s_pm_lock);
                s_running = false;

                /* Mute через power (XSMT) */
                power_audio_pipeline_stopped();
                ESP_LOGI(TAG, "Пайплайн зупинено, mute виконано");
            }
        }

        /* ── РОБОЧИЙ ЦИКЛ (hot-path, жодних логів) ── */
        if (s_running) {
            size_t bytes_read = 0, bytes_written = 0;

            i2s_channel_read(s_rx_chan, rx_buf, AUDIO_BLOCK_BYTES,
                             &bytes_read, portMAX_DELAY);

            int64_t t_work = esp_timer_get_time();

            /*
             * Повний DSP-ланцюжок (audio-B):
             *   1) DC-blocker (HPF 7 Hz)
             *   2) Per-source trim
             *   3) 10-смуговий графічний EQ (peaking biquad)
             *   4) Гучність із ramp 30 мс
             *   5) Лімітер (стеля -1 dBFS)
             *
             * Жодних динамічних алокацій. Коефіцієнти біквадів
             * оновлюються ТІЛЬКИ через API (подвійне буферування).
             */
            audio_process_block(rx_buf, tx_buf, AUDIO_BLOCK_FRAMES);

            i2s_channel_write(s_tx_chan, tx_buf, AUDIO_BLOCK_BYTES,
                              &bytes_written, portMAX_DELAY);

            if (bytes_read    < AUDIO_BLOCK_BYTES) s_stats_overrun++;
            if (bytes_written < AUDIO_BLOCK_BYTES) s_stats_underrun++;

            /*
             * CPU load: EMA зайнятого часу блоку відносно періоду
             * блоку (5333 мкс @ 48 kHz / 256 фреймів).
             */
            int64_t dt = esp_timer_get_time() - t_work;
            s_busy_us_ema = (s_busy_us_ema * 7 + dt * 3) / 10;
            s_stats_cpu_load = (uint32_t)(
                (s_busy_us_ema * 100) /
                (AUDIO_BLOCK_FRAMES * 1000000 / BSP_AUDIO_SAMPLE_RATE_HZ));
        }
    }
}