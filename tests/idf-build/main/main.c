#include "dc_bambu.h"
#include "dc_evlog.h"
#include "dc_moonraker.h"
#include "dc_source.h"
#include "dc_ui.h"
#include "dc_wifi.h"

// This application exists only to compile and link every public component API
// together. Runtime behavior remains the responsibility of product firmware.
void app_main(void)
{
    (void)dc_ui_spa_asset();
}
