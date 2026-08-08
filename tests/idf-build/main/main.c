#include "dc_bambu.h"
#include "dc_evlog.h"
#include "dc_moonraker.h"
#include "dc_mqtt.h"
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
    (void)dc_mqtt_start(&mqtt_config, &mqtt);
    (void)dc_mqtt_destroy(mqtt);
    (void)dc_ui_spa_asset();
}
