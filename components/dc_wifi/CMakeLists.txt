idf_component_register(
    SRCS "dc_wifi.c" "dc_wifi_validate.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event mdns lwip
    PRIV_REQUIRES esp_timer
)
