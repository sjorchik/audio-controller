idf_component_register(SRCS "audio.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bsp power freertos esp_driver_i2s esp_pm esp_timer)
