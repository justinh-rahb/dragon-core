#include "dc_source.h"
#include "nvs.h"

#define NVS_NS  "app_nvs"
#define KEY_SRC "ctl_src"

dc_ctl_source_t dc_source_get(void)
{
    uint8_t v = DC_SRC_KLIPPER;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_SRC, &v);   // leaves v at default if key absent
        nvs_close(h);
    }
    if (v > DC_SRC_MAX) v = DC_SRC_KLIPPER;   // fail-safe to the shipped path
    return (dc_ctl_source_t)v;
}

esp_err_t dc_source_set(dc_ctl_source_t src)
{
    if (src > DC_SRC_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, KEY_SRC, (uint8_t)src);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

const char *dc_source_str(dc_ctl_source_t src)
{
    switch (src) {
    case DC_SRC_BAMBU:        return "bambu";
    case DC_SRC_HA:           return "ha";
    case DC_SRC_KLIPPER_MQTT: return "klipper-mqtt";
    case DC_SRC_NONE:         return "none";
    default:                  return "klipper";
    }
}
