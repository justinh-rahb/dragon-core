idf_component_register(
    SRCS "dc_bambu.c" "dc_bambu_discovery.c"
    INCLUDE_DIRS "include"
    # esp-mqtt over TLS (esp-tls/mbedTLS) to the printer's on-device broker.
    # Reports are scanned with strstr/strtof (no cJSON). Built into ESP-IDF.
    # dc_bambu_discovery.c uses lwip sockets + esp_netif (STA multicast join) and
    # esp_timer for the scan window / age-out timestamps.
    REQUIRES nvs_flash mqtt esp-tls esp_netif esp_timer
)
