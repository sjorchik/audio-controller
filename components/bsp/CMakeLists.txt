idf_component_register(
    SRCS "bsp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common log
    PRIV_REQUIRES driver
)
