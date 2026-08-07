# Aggregate ESP-IDF component for the Dragon firmware family.
# Consumers add dragon-core once; the manifest resolves the sibling dc_* components.
idf_component_register(
    REQUIRES
        dc_evlog
        dc_source
        dc_bambu
        dc_wifi
        dc_moonraker
)
