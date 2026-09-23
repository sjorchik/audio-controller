# Аудіоконтролер на ESP32-S3

Цифровий аудіопроцесор із вибором джерел, еквалайзером і web-керуванням.

## Огляд

Пристрій приймає 4 аналогові джерела (TV BOX, Computer, Bluetooth, AUX),
комутує їх через селектор TDA7318, оцифровує через PCM1808, обробляє в
ESP32-S3 (гучність, 10-смуговий EQ, лімітер) і виводить через PCM5102 на
підсилювач.

Керування: енкодер з кнопкою, 6 кнопок, IR-пульт (RC5), Web UI українською.

## Специфікація v2.2

### Платформа

- **MCU**: ESP32-S3-N16R8
  - 16 MB QIO flash
  - 8 MB octal PSRAM
- **Framework**: ESP-IDF 5.5.0 (без Arduino)
- **PlatformIO**: platform espressif32 6.12.0, toolchain 14.2.0

### Периферія

| Компонент | Призначення | Інтерфейс |
|---|---|---|
| TDA7318 | Селектор входів (тільки комутація, без гучності/тону) | I2C |
| PCM1808 | АЦП (slave по всіх тактах) | I2S RX |
| PCM5102 | ЦАП (без MCLK, mute через XSMT) | I2S TX |
| ST7789 170x320 | Дисплей 1.9" IPS | SPI |
| VS1838B | IR-приймач | RMT RX |
| Енкодер | Обертання + кнопка | PCNT + GPIO |
| 6 кнопок | POWER, UP, DOWN, LEFT, RIGHT, OK | GPIO |

### Аудіотракт

Шлях сигналу:

    Джерела → TDA7318 (селектор) → PCM1808 → ESP32-S3 (DSP) → PCM5102 → підсилювач

DSP-ланцюжок:

1. DC-blocker (HPF 5–10 Гц)
2. Per-source trim (компенсація рівнів джерел)
3. 10-смуговий графічний EQ (31.5–16k Гц, ±12 dB, пресети)
4. Гучність -90…0 dB з плавним ramp (без zipper-шуму)
5. Лімітер/soft-clip -1 dBFS

Тактування:

- ESP32-S3 — I2S-master, full-duplex, один контролер
- fs = 48 kHz (фіксовано)
- BCLK = 64×fs = 3.072 MHz (спільний для PCM1808 і PCM5102)
- WS/LRCLK = 48 kHz (спільний)
- MCLK = 256×fs = 12.288 MHz (тільки на SCKI PCM1808)
- PCM1808 — slave по всіх тактах
- PCM5102 — без MCLK
- Slot 32 bit, дані 24 bit MSB-aligned, формат I2S

### Режими роботи

- **BOOT**: PCM5102 у mute (XSMT Low) до готовності аудіопайплайну
- **RUN**: нормальна робота, Wi-Fi активний
- **STANDBY**: м'який standby
  - CPU живий, RTOS активний
  - Wi-Fi вимкнений повністю
  - Дисплей і підсвітка вимкнені
  - I2S-такти зупинені
  - PCM5102 у mute через XSMT
  - TDA7318 у mute
  - Частота CPU знижена через esp_pm
  - Wake тільки: кнопка POWER або RC5-команда power
- **ERROR**: детект помилок (I2C/I2S fail), mute, індикація, спроба відновлення

### Додаткові можливості

- **Wi-Fi**: STA + AP captive portal provisioning
- **Web UI**: українською, HTTP REST + WebSocket, mDNS (audioctrl.local)
- **IR-пульт**: RC5 + режим навчання (прив'язка кодів до дій через Web)
- **NVS**: збереження джерела, гучності, EQ, trim, IR-карти, Wi-Fi, останнього стану
- **OTA**: закладено в partition table (фаза 2)

## PINMAP v2.2

| Блок | Сигнал | GPIO | Примітка |
|---|---|---:|---|
| ST7789 SPI | SCLK | 12 | SPI2 |
| | MOSI | 10 | |
| | CS | 9 | |
| | DC | 11 | |
| | RST | 13 | |
| | BLK | 14 | LEDC PWM |
| I2S | MCLK → PCM1808 SCKI | 16 | 12.288 MHz |
| | BCLK | 15 | 3.072 MHz |
| | WS/LRCLK | 17 | 48 kHz |
| | DOUT → PCM5102 DIN | 18 | |
| | DIN ← PCM1808 DOUT | 8 | |
| I2C | SDA | 1 | TDA7318 |
| | SCL | 2 | TDA7318 |
| Енкодер | A | 4 | PCNT |
| | B | 5 | PCNT ctrl |
| | BTN | 6 | |
| Кнопки | POWER | 7 | |
| | UP | 21 | |
| | DOWN | 38 | |
| | LEFT | 39 | |
| | RIGHT | 40 | |
| | OK | 41 | |
| IR | VS1838B OUT | 47 | RMT RX |
| Резерв BT | UART1 TX | 42 | |
| | UART1 RX | 48 | |
| Console | UART0 TX | 43 | |
| | UART0 RX | 44 | |
| PCM5102 | XSMT (mute) | 45 | Active-low, зовнішній pulldown 10 kΩ |

Не використовувати: 0, 3, 46 (strapping / input-only), 26–32 (flash),
33–37 (PSRAM), 19/20 (USB). GPIO45 використано під XSMT свідомо,
обґрунтування див. у components/bsp/include/bsp.h.

Апаратна вимога: зовнішній pulldown 10 kΩ на лінії XSMT гарантує mute у
reset/high-Z (до ініціалізації GPIO). Модуль PCM5102 не повинен мати
конфліктної підтяжки XSMT до VCC.

## Структура проєкту

    audio-controller/
    ├── components/
    │   ├── bsp/           # Board Support Package: pinmap, базова периферія
    │   ├── system/        # Шина подій, утиліти
    │   ├── settings/      # NVS: налаштування, IR-карта, Wi-Fi
    │   ├── power/         # State machine: BOOT/RUN/STANDBY/ERROR
    │   ├── audio/         # I2S-транспорт + DSP
    │   ├── tda7318/       # Драйвер селектора входів
    │   ├── input/         # Кнопки + енкодер
    │   ├── ir/            # IR-приймач RC5 + навчання
    │   ├── ui/            # Дисплей + LVGL
    │   └── web/           # Wi-Fi + HTTP + WebSocket + mDNS
    ├── src/
    │   └── main.c         # app_main: ініціалізація і створення задач
    ├── include/           # Загальні заголовки
    ├── test/              # Юніт-тести
    ├── partitions.csv     # OTA-схема + storage (LittleFS)
    ├── platformio.ini     # Конфігурація PlatformIO
    └── sdkconfig.defaults # Базові налаштування ESP-IDF

## Завдання FreeRTOS

| Задача | Core | Пріоритет | Функція |
|---|---:|---:|---|
| audio | 1 | високий | I2S DMA + DSP-ланцюжок |
| ctrl | 0 | середній | State machine, TDA I2C, послідовності mute/switch |
| input | 0 | середній | Кнопки/енкодер/IR → черга подій |
| ui | 0 | середній-низький | LVGL tick + рендер + SPI DMA flush |
| web | 0 | низький-середній | HTTP/WS, mDNS, provisioning |

## Команди

    # Збірка
    pio run

    # Прошивка
    pio run -t upload

    # Монітор
    pio device monitor

    # Clean rebuild (після змін sdkconfig.defaults)
    pio run -t clean
    pio run

## Дорожня карта

Див. docs/ROADMAP.md

## Ліцензія

MIT