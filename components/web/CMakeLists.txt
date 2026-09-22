idf_component_register(
    SRCS "web.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp system esp_common log
    PRIV_REQUIRES esp_wifi esp_http_server esp_netif nvs_flash
)
