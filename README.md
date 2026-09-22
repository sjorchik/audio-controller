# Аудіоконтролер на ESP32-S3

Проєкт аудіоконтролера на базі:

- ESP32-S3-N16R8:
  - 16 MB QIO flash;
  - 8 MB octal PSRAM;
- селектор входів TDA7318 по I2C;
- АЦП PCM1808 по I2S;
- DSP всередині ESP32-S3;
- ЦАП PCM5102 по I2S;
- дисплей 1.9" IPS ST7789 170x320 по SPI;
- енкодер з кнопкою;
- 6 кнопок;
- IR-приймач VS1838B, протокол RC5;
- Wi-Fi STA + AP/captive portal у майбутньому;
- Web UI українською.

## Платформа

- Тільки ESP-IDF.
- Без Arduino.
- Збірка через PlatformIO.

## Команди

Збірка:

```bash
pio run