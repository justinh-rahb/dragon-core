#include "dc_bambu.h"
#include "dc_evlog.h"
#include "dc_lighting.h"
#include "dc_moonraker.h"
#include "dc_mqtt.h"
#include "dc_portal.h"
#include "dc_prusa.h"
#include "dc_source.h"
#include "dc_ui.h"
#include "dc_wifi.h"

// This application exists only to compile and link every public component API
// together. Runtime behavior remains the responsibility of product firmware.
void app_main(void)
{
    // Invalid arguments exercise and link-check lifecycle entry points without
    // starting a broker connection in the compile-test application.
    dc_mqtt_client_t *mqtt = NULL;
    dc_mqtt_config_t mqtt_config = {0};
    dc_bambu_config_t bambu_config = {0};
    dc_moonraker_config_t moonraker_config = {0};
    dc_prusa_config_t prusa_config = {0};
    dc_prusa_status_t prusa_status = {0};
    dc_lighting_output_t lighting_output = {0};
    dc_lighting_stats_t lighting_stats = {0};
    // Config APIs are deliberately valid before either client is started. Shared
    // provisioning uses this to edit inactive sources without losing saved fields.
    (void)dc_bambu_get_config(&bambu_config);
    (void)dc_bambu_set_config(&bambu_config);
    (void)dc_moonraker_get_config(&moonraker_config);
    (void)dc_moonraker_set_config(&moonraker_config);
    (void)dc_prusa_get_config(&prusa_config);
    (void)dc_prusa_set_config(&prusa_config);
    (void)dc_prusa_get_status(&prusa_status);
    (void)dc_mqtt_start(&mqtt_config, &mqtt);
    (void)dc_mqtt_destroy(mqtt);
    (void)dc_lighting_start(&(dc_lighting_config_t){ .outputs = &lighting_output });
    dc_lighting_get_stats(&lighting_stats);
    (void)dc_lighting_set_brightness(0);
    (void)dc_lighting_set_output_reverse(0, false);
    (void)dc_ui_spa_asset();
    (void)dc_portal_start(NULL);
    (void)dc_portal_httpd();
}
