idf_component_register(
    SRCS "tda7318.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp system esp_common log
    PRIV_REQUIRES driver audio
)