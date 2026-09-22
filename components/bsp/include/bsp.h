#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// PINMAP v2. Усі компоненти використовують ТІЛЬКИ ці макроси.

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

// Енкодер
#define BSP_PIN_ENCODER_A        4
#define BSP_PIN_ENCODER_B        5
#define BSP_PIN_ENCODER_BTN      6

// Кнопки
#define BSP_PIN_BTN_POWER        7
#define BSP_PIN_BTN_UP           21
#define BSP_PIN_BTN_DOWN         38
#define BSP_PIN_BTN_LEFT         39
#define BSP_PIN_BTN_RIGHT        40
#define BSP_PIN_BTN_OK           41

// IR
#define BSP_PIN_IR_OUT           47

// Резерв BT UART1
#define BSP_PIN_BT_UART_TX       42
#define BSP_PIN_BT_UART_RX       48

// Console UART0
#define BSP_PIN_CONSOLE_TX       43
#define BSP_PIN_CONSOLE_RX       44

// PCM5102 XSMT (mute), специфікація v2.2.
// Вибір піна: GPIO45.
//  - GPIO22-25 НЕ розведені на ESP32-S3-WROOM-1/DevKitC-1 (відсутні у pin-листі модуля);
//  - GPIO46 - input-only;
//  - GPIO0/GPIO3 - strapping boot-режимів, неприпустимі для сигналу,
//    який має бути Low на старті;
//  - GPIO19/20 - резерв USB;
//  - strapping-функція GPIO45 (VDD_SPI) не конфліктує: нам потрібен Low на старті,
//    що збігається з безпечним strapping-станом.
// АПАРАТНА ВИМОГА: зовнішній pulldown 10 kOhm на лінії XSMT. Він тримає mute,
// поки ESP32 у reset або GPIO ще не ініціалізований (high-Z), тобто гарантує
// відсутність клацань при старті й перепрошивці до будь-якого коду.
// Модуль PCM5102 НЕ повинен мати конфліктної підтяжки XSMT до VCC.
// Власник сигналу: виключно компонент power (BOOT-послідовність).
#define BSP_PIN_PCM5102_XSMT     45

// Рівень активної кнопки (active low, підтяжка до +)
#define BSP_INPUT_ACTIVE_LEVEL   0

// Параметри аудіотракту, специфікація v2.2, фіксовано для fw v1.
// ESP32-S3 - I2S-master, full-duplex, один контролер.
#define BSP_AUDIO_SAMPLE_RATE_HZ 48000   // fs, фіксовано
#define BSP_AUDIO_SLOT_BIT_WIDTH 32      // слот 32 bit
#define BSP_AUDIO_DATA_BIT_WIDTH 24      // дані 24 bit MSB-aligned
#define BSP_AUDIO_BCLK_FS        64      // BCLK = 64*fs = 3.072 MHz, спільний для PCM1808 і PCM5102
#define BSP_AUDIO_MCLK_FS        256     // MCLK = 256*fs = 12.288 MHz, лише на SCKI PCM1808

// Джерела аудіо
typedef enum {
    BSP_AUDIO_SOURCE_TV_BOX = 0,
    BSP_AUDIO_SOURCE_COMPUTER,
    BSP_AUDIO_SOURCE_BLUETOOTH,
    BSP_AUDIO_SOURCE_AUX,
    BSP_AUDIO_SOURCE_MAX
} bsp_audio_source_t;

esp_err_t bsp_init(void);
esp_err_t bsp_deinit(void);

// Керування mute ЦАП (PCM5102 XSMT): mute=true -> XSMT Low.
// ЄДИНА обгортка запису у BSP_PIN_PCM5102_XSMT.
// Викликати має ТІЛЬКИ компонент power у BOOT-послідовності.
esp_err_t bsp_audio_set_dac_mute(bool mute);