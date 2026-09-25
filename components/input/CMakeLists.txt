idf_component_register(
    SRCS "input.c" "button_sm.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp system esp_common log
    PRIV_REQUIRES driver esp_timer console
)