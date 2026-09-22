idf_component_register(
    SRCS "power.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp system esp_common log
    PRIV_REQUIRES esp_pm driver
)
