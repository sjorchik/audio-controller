idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        bsp
        system
        settings
        power
        audio
        tda7318
        input
        ir
        ui
        web
        nvs_flash
        esp_system
        spi_flash
        esp_psram
        heap
        log
)
