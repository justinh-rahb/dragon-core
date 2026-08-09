idf_component_register(
    SRCS "dc_portal.c" "dc_portal_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_netif esp_wifi app_update esp_partition lwip json mbedtls
             dc_wifi dc_ui dc_evlog
)
