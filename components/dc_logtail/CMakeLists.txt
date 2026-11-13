idf_component_register(
    SRCS "dc_logtail.c" "dc_logtail_contract.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES dc_evlog
)
