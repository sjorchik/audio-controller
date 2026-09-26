# Файл: components/ui/CMakeLists.txt
idf_component_register(
    SRCS "ui.c" "font.c"
    INCLUDE_DIRS "include"
    # bsp   — піни ST7789/LEDC;
    # system — шина подій (ui_event_t, system_register_queue);
    # audio/tda7318 — HMI читає стан геттерами і кличе сетери напряму
    #   (архітектура v2.2.5);
    # esp_timer — LVGL tick 5 мс; esp_lcd — панель ST7789.
    REQUIRES bsp system audio tda7318 esp_common log esp_timer esp_lcd
    # driver — SPI master + LEDC; lvgl — managed component (idf_component.yml).
    PRIV_REQUIRES driver lvgl
)