idf_component_register(
    SRCS "settings.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp system esp_common log
    PRIV_REQUIRES nvs_flash
)
