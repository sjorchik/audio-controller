#include "tda7318.h"
#include "bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// audio_set_mute реалізована в audio_process.c, але не експортується в audio.h.
// Оголошуємо тут, щоб не модифікувати компонент audio/.
extern void audio_set_mute(bool mute);

static const char *TAG = "tda7318";

/* ======================================================================
 * КАТА РЕГІСТРІВ родини TDA7313/TDA7318/PT2313.
 * Джерела: (1) робоча бібліотека PT2313 v1.0 (M. Costa);
 *          (2) емпіричні проби dev-командою 'raw' на стенді.
 *
 * Volume:      0x00|att, att=0..63 кроків по 1.25 dB; 0x00 = 0 dB.
 *              Біта mute НЕМАЄ: mute = код 0x3F (-78.75 dB, фактично глухо).
 * Function:    0x40 | gain(b4,b3) | loudness(b2) | input(b1,b0).
 *              gain 0 dB = b4,b3 = 1,1 (0x18); loudness OFF = b2 = 1 (0x04);
 *              отже базовий байт = 0x5C, вхід = біти 0-1.
 * Bass/Treble: 0x60|n, 0x70|n; flat (0 dB) = n = 15 -> 0x6F / 0x7F.
 * Attenuators: 0x80(R1)/0xA0(L1?)/0xC0(L)/0xE0(R) | att(5 bit), 0 = 0 dB.
 *              (у нашому тракті не використовуються: баланс = 0 dB)
 * ====================================================================== */
#define CHIP_VOL_BASE         0x00
#define CHIP_VOL_0DB          (CHIP_VOL_BASE | 0x00)   // 0 dB, специфікація v2
#define CHIP_VOL_MUTE         (CHIP_VOL_BASE | 0x3F)   // -78.75 dB = mute
#ifdef CONFIG_APP_SELECTOR_CHIP_PT2313
#define CHIP_NAME             "PT2313"
// Бібліотека PT2313 v1.0 (M. Costa): flat = нібл 15; входи val % 3 -> 3 входи
#define CHIP_BASS_FLAT        0x6F
#define CHIP_TREBLE_FLAT      0x7F
#define CHIP_INPUT_COUNT      3
#else
#define CHIP_NAME             "TDA7318"
// Робочий проєкт TDA7318 (SoundCube/WifiRadio): flat = нібл 7
// (±14 dB, крок 2 dB, центр 7); 4 входи
#define CHIP_BASS_FLAT        0x67
#define CHIP_TREBLE_FLAT      0x77
#define CHIP_INPUT_COUNT      4
#endif
#define CHIP_ATT_R1           0x80
#define CHIP_ATT_L1           0xA0
#define CHIP_ATT_L            0xC0
#define CHIP_ATT_R            0xE0
#define CHIP_ATT_0DB          0x00
#define CHIP_FUNC_BASE        0x40
#define CHIP_FUNC_GAIN_0DB    0x18   // b4,b3 = 1,1
#define CHIP_FUNC_LOUD_OFF    0x04   // b2 = 1
#define CHIP_FUNC_SRC_MASK    0x03
#define CHIP_FUNC_DEFAULT     (CHIP_FUNC_BASE | CHIP_FUNC_GAIN_0DB | CHIP_FUNC_LOUD_OFF) /* 0x5C */

#ifdef CONFIG_APP_SELECTOR_CHIP_PT2313
#define CHIP_NAME             "PT2313"
// Бібліотека PT2313 обмежує входи val % 3 -> ймовірно 3 входи.
// Якщо проба 'raw 0x5F' з сигналом на IN3 перемикає — поставити 4.
#define CHIP_INPUT_COUNT      3
#else
#define CHIP_NAME             "TDA7318"
#define CHIP_INPUT_COUNT      4
#endif

static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;
static i2c_master_dev_handle_t s_dev_handle = NULL;
static uint32_t s_i2c_errors = 0;
static bsp_audio_source_t s_current_input = BSP_AUDIO_SOURCE_TV_BOX;
static bool s_is_muted = true;

/** Запис байта: таймаут 100 мс, 1 retry; помилка -> лічильник + esp_err. */
static esp_err_t chip_write_byte(uint8_t cmd) {
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < 2; i++) {
        ret = i2c_master_transmit(s_dev_handle, &cmd, 1, 100);
        if (ret == ESP_OK) break;
    }
    if (ret != ESP_OK) {
        s_i2c_errors++;
        ESP_LOGE(TAG, "I2C помилка транзакції: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t tda7318_init(void) {
    ESP_LOGI(TAG, "Ініціалізація %s (адреса 0x%02X, 400 kHz)",
             CHIP_NAME, TDA7318_I2C_ADDR);

    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .scl_io_num = BSP_PIN_I2C_SCL,   // піни ТІЛЬКИ з bsp.h (SCL=2)
        .sda_io_num = BSP_PIN_I2C_SDA,   // (SDA=1)
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &s_i2c_bus_handle));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TDA7318_I2C_ADDR,
        .scl_speed_hz = 400000,          // 400 kHz
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus_handle, &dev_cfg, &s_dev_handle));

    // Фіксовані значення специфікації v2: volume 0 dB, tone flat, loudness off,
    // баланс 0 dB. Помилки НЕ фатальні: лічимо, повертаємо esp_err наверх
    // (у пункті 8 power переведе систему у ERROR).
    esp_err_t first_err = ESP_OK, err;
    static const uint8_t init_seq[] = {
        CHIP_BASS_FLAT,                       // tone flat: bass 0 dB
        CHIP_TREBLE_FLAT,                     // tone flat: treble 0 dB
        CHIP_ATT_R1 | CHIP_ATT_0DB,           // баланс/атенюатори: 0 dB
        CHIP_ATT_L1 | CHIP_ATT_0DB,
        CHIP_ATT_L  | CHIP_ATT_0DB,
        CHIP_ATT_R  | CHIP_ATT_0DB,
        CHIP_FUNC_DEFAULT | (BSP_AUDIO_SOURCE_TV_BOX & CHIP_FUNC_SRC_MASK), // IN0
        CHIP_VOL_0DB,                         // ЯВНИЙ запис volume 0 dB
    };
    for (size_t i = 0; i < sizeof(init_seq); i++) {
        err = chip_write_byte(init_seq[i]);
        if (err != ESP_OK && first_err == ESP_OK) first_err = err;
    }

    // Стан після init = mute (біта mute у чіпа немає -> код volume 0x3F)
    err = chip_write_byte(CHIP_VOL_MUTE);
    if (err != ESP_OK && first_err == ESP_OK) first_err = err;

    if (first_err != ESP_OK) {
        ESP_LOGE(TAG, "%s не відповідає: %s. Діагностика: i2cscan; "
                      "перевірте живлення, pull-up, SDA=GPIO1/SCL=GPIO2, GND",
                 CHIP_NAME, esp_err_to_name(first_err));
        return first_err;
    }

    s_is_muted = true;
    s_current_input = BSP_AUDIO_SOURCE_TV_BOX;
    ESP_LOGI(TAG, "Ініціалізація завершена (%s): volume 0dB, tone flat, "
                  "loudness off, mute ON", CHIP_NAME);
    return ESP_OK;
}

esp_err_t tda7318_select_source(bsp_audio_source_t source) {
    if ((int)source >= CHIP_INPUT_COUNT) {
        // На стендовому PT2313 (3 входи) IN3 фізично відсутній
        return ESP_ERR_NOT_SUPPORTED;
    }
    // Мапа: TV_BOX=IN0, COMPUTER=IN1, BLUETOOTH=IN2, AUX=IN3
    // Function-байт: gain 0 dB, loudness off, вхід = біти 0-1
    s_current_input = source;
    return chip_write_byte(CHIP_FUNC_DEFAULT | ((uint8_t)source & CHIP_FUNC_SRC_MASK));
}

esp_err_t tda7318_set_mute(bool mute) {
    // Біта mute у родини немає: mute = код volume 0x3F (-78.75 dB).
    // Безкліковість у switch_source гарантує DSP-ramp; окремий виклик
    // (STANDBY, пункт 8) має викликатися ПОЗА рампою DSP або після неї.
    s_is_muted = mute;
    return chip_write_byte(mute ? CHIP_VOL_MUTE : CHIP_VOL_0DB);
}

esp_err_t tda7318_switch_source(bsp_audio_source_t source) {
    if ((int)source >= CHIP_INPUT_COUNT) return ESP_ERR_NOT_SUPPORTED;
    if (source == s_current_input && !s_is_muted) return ESP_OK;

    ESP_LOGI(TAG, "Перемикання на джерело %d", (int)source);

    // Безклікова послідовність (контекст задачі, не ISR):
    audio_set_mute(true);            // DSP ramp down 30 мс
    vTaskDelay(pdMS_TO_TICKS(35));   // +5 мс запас на RTOS
    esp_err_t ret = tda7318_select_source(source);
    if (ret != ESP_OK) {
        audio_set_mute(false);       // не лишати тракт глухим після збою I2C
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(30));   // аналогове заспокоєння комутатора
    audio_set_mute(false);           // DSP ramp up 30 мс

    ESP_LOGI(TAG, "Перемикання завершено");
    return ESP_OK;
}

void tda7318_get_stats(tda7318_stats_t *stats) {
    if (stats) stats->i2c_errors = s_i2c_errors;
}

i2c_master_bus_handle_t tda7318_get_i2c_bus(void) {
    return s_i2c_bus_handle;
}

esp_err_t tda7318_raw_write(uint8_t cmd) {
    return chip_write_byte(cmd);
}