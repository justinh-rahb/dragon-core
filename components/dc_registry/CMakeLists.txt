idf_component_register(
    SRCS "dc_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer dc_peer
)
