idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp system esp_common log
    PRIV_REQUIRES driver
)
