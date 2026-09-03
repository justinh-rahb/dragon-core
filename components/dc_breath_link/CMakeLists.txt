idf_component_register(
    SRCS "dc_breath_link.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_http_client esp_timer json
)
