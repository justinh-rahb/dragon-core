idf_component_register(
    SRCS "dc_moonraker.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash json esp_websocket_client dc_evlog
)
