// Файл: components/tda7318/tda7318.c
#include "tda7318.h"
#include "bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "system.h"

extern void audio_set_mute(bool mute);

static const char *TAG = "tda7318";

#define CHIP_VOL_BASE 0x00
#define CHIP_VOL_0DB (CHIP_VOL_BASE | 0x00)
#define CHIP_VOL_MUTE (CHIP_VOL_BASE | 0x3F)
#ifdef CONFIG_APP_SELECTOR_CHIP_PT2313
#define CHIP_NAME "PT2313"
#define CHIP_BASS_FLAT 0x6F
#define CHIP_TREBLE_FLAT 0x7F
#define CHIP_INPUT_COUNT 3
#else
#define CHIP_NAME "TDA7318"
#define CHIP_BASS_FLAT 0x67
#define CHIP_TREBLE_FLAT 0x77
#define CHIP_INPUT_COUNT 4
#endif
#define CHIP_ATT_R1 0x80
#define CHIP_ATT_L1 0xA0
#define CHIP_ATT_L 0xC0
#define CHIP_ATT_R 0xE0
#define CHIP_ATT_0DB 0x00
#define CHIP_FUNC_BASE 0x40
#define CHIP_FUNC_GAIN_0DB 0x18
#define CHIP_FUNC_LOUD_OFF 0x04
#define CHIP_FUNC_SRC_MASK 0x03
#define CHIP_FUNC_DEFAULT (CHIP_FUNC_BASE | CHIP_FUNC_GAIN_0DB | CHIP_FUNC_LOUD_OFF)

static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;
static i2c_master_dev_handle_t s_dev_handle = NULL;
static uint32_t s_i2c_errors = 0;
static bsp_audio_source_t s_current_input = BSP_AUDIO_SOURCE_TV_BOX;
static bool s_is_muted = true;

static esp_err_t chip_write_byte(uint8_t cmd) {
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < 2; i++) {
        ret = i2c_master_transmit(s_dev_handle, &cmd, 1, 100);
        if (ret == ESP_OK) break;
    }
    if (ret != ESP_OK) { s_i2c_errors++; ESP_LOGE(TAG, "I2C помилка транзакції: %s", esp_err_to_name(ret)); }
    return ret;
}

esp_err_t tda7318_init(void) {
    ESP_LOGI(TAG, "Ініціалізація %s", CHIP_NAME);
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = -1,
        .scl_io_num = BSP_PIN_I2C_SCL, .sda_io_num = BSP_PIN_I2C_SDA,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &s_i2c_bus_handle));
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TDA7318_I2C_ADDR, .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus_handle, &dev_cfg, &s_dev_handle));

    esp_err_t first_err = ESP_OK, err;
    static const uint8_t init_seq[] = {
        CHIP_BASS_FLAT, CHIP_TREBLE_FLAT, CHIP_ATT_R1 | CHIP_ATT_0DB, CHIP_ATT_L1 | CHIP_ATT_0DB,
        CHIP_ATT_L | CHIP_ATT_0DB, CHIP_ATT_R | CHIP_ATT_0DB,
        CHIP_FUNC_DEFAULT | (BSP_AUDIO_SOURCE_TV_BOX & CHIP_FUNC_SRC_MASK), CHIP_VOL_0DB,
    };
    for (size_t i = 0; i < sizeof(init_seq); i++) {
        err = chip_write_byte(init_seq[i]);
        if (err != ESP_OK && first_err == ESP_OK) first_err = err;
    }
    err = chip_write_byte(CHIP_VOL_MUTE);
    if (err != ESP_OK && first_err == ESP_OK) first_err = err;
    if (first_err != ESP_OK) return first_err;

    s_is_muted = true; s_current_input = BSP_AUDIO_SOURCE_TV_BOX;
    return ESP_OK;
}

esp_err_t tda7318_select_source(bsp_audio_source_t source) {
    if ((int)source >= CHIP_INPUT_COUNT) return ESP_ERR_NOT_SUPPORTED;
    s_current_input = source;
    return chip_write_byte(CHIP_FUNC_DEFAULT | ((uint8_t)source & CHIP_FUNC_SRC_MASK));
}

esp_err_t tda7318_set_mute(bool mute) {
    s_is_muted = mute;
    return chip_write_byte(mute ? CHIP_VOL_MUTE : CHIP_VOL_0DB);
}

esp_err_t tda7318_switch_source(bsp_audio_source_t source) {
    if ((int)source >= CHIP_INPUT_COUNT) return ESP_ERR_NOT_SUPPORTED;
    if (source == s_current_input && !s_is_muted) return ESP_OK;

    audio_set_mute(true);
    vTaskDelay(pdMS_TO_TICKS(35));
    esp_err_t ret = tda7318_select_source(source);
    if (ret != ESP_OK) { audio_set_mute(false); return ret; }
    vTaskDelay(pdMS_TO_TICKS(30));
    audio_set_mute(false);

    system_post(SYSTEM_EVENT_SOURCE_CHANGED, NULL, 0);
    return ESP_OK;
}

void tda7318_get_stats(tda7318_stats_t *stats) { if (stats) stats->i2c_errors = s_i2c_errors; }
i2c_master_bus_handle_t tda7318_get_i2c_bus(void) { return s_i2c_bus_handle; }
esp_err_t tda7318_raw_write(uint8_t cmd) { return chip_write_byte(cmd); }
bsp_audio_source_t tda7318_get_source(void) { return s_current_input; }