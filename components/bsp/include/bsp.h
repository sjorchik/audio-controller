#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// PINMAP v2. сі компоненти використовують ТЬ ці макроси.

// ST7789 SPI
#define BSP_PIN_ST7789_SCLK      12
#define BSP_PIN_ST7789_MOSI      10
#define BSP_PIN_ST7789_CS        9
#define BSP_PIN_ST7789_DC        11
#define BSP_PIN_ST7789_RST       13
#define BSP_PIN_ST7789_BLK       14

// I2S
#define BSP_PIN_I2S_MCLK         16
#define BSP_PIN_I2S_BCLK         15
#define BSP_PIN_I2S_WS           17
#define BSP_PIN_I2S_DOUT         18
#define BSP_PIN_I2S_DIN          8

// I2C, TDA7318
#define BSP_PIN_I2C_SDA          1
#define BSP_PIN_I2C_SCL          2

// нкодер
#define BSP_PIN_ENCODER_A        4
#define BSP_PIN_ENCODER_B        5
#define BSP_PIN_ENCODER_BTN      6

// нопки
#define BSP_PIN_BTN_POWER        7
#define BSP_PIN_BTN_UP           21
#define BSP_PIN_BTN_DOWN         38
#define BSP_PIN_BTN_LEFT         39
#define BSP_PIN_BTN_RIGHT        40
#define BSP_PIN_BTN_OK           41

// IR
#define BSP_PIN_IR_OUT           47

// езерв BT UART1
#define BSP_PIN_BT_UART_TX       42
#define BSP_PIN_BT_UART_RX       48

// Console UART0
#define BSP_PIN_CONSOLE_TX       43
#define BSP_PIN_CONSOLE_RX       44

// івень активної кнопки (active low, підтяжка до +)
#define BSP_INPUT_ACTIVE_LEVEL   0

// жерела аудіо
typedef enum {
    BSP_AUDIO_SOURCE_TV_BOX = 0,
    BSP_AUDIO_SOURCE_COMPUTER,
    BSP_AUDIO_SOURCE_BLUETOOTH,
    BSP_AUDIO_SOURCE_AUX,
    BSP_AUDIO_SOURCE_MAX
} bsp_audio_source_t;

esp_err_t bsp_init(void);
esp_err_t bsp_deinit(void);
