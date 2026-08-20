idf_component_register(
    SRCS "dc_prusa.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_http_client json
)
