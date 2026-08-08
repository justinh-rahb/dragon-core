#pragma once

// Thin Dragon-family wrapper around ESP-MQTT. Products and protocol components own
// topic schemas, payloads, persistence, pacing, and control policy; this component
// owns only client lifecycle and the common connect/disconnect/message seam.

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dc_mqtt_client dc_mqtt_client_t;

typedef enum {
    DC_MQTT_EVENT_CONNECTED,
    DC_MQTT_EVENT_DISCONNECTED,
    DC_MQTT_EVENT_MESSAGE,
    DC_MQTT_EVENT_ERROR,
} dc_mqtt_event_type_t;

typedef struct {
    dc_mqtt_event_type_t type;
    dc_mqtt_client_t *client;
    const char *topic;  // valid only during a MESSAGE callback; not NUL-terminated
    int topic_len;
    const char *data;   // valid only during a MESSAGE callback; not NUL-terminated
    int data_len;
    int total_data_len;       // payload may span more than one callback
    int current_data_offset;
} dc_mqtt_event_t;

typedef void (*dc_mqtt_event_cb_t)(void *ctx, const dc_mqtt_event_t *event);

typedef struct {
    const char *broker_uri;  // mqtt://host:port or mqtts://host:port
    const char *username;    // optional
    const char *password;    // optional

    const char *last_will_topic;   // optional; all last-will fields ignored if NULL
    const char *last_will_message;
    int last_will_qos;
    bool last_will_retain;

    // Compatibility option for LAN brokers with a trusted chain but a certificate
    // name that does not match the local hostname. Products should leave this false
    // whenever their broker certificate has a correct DNS/IP identity.
    bool skip_cert_common_name_check;

    dc_mqtt_event_cb_t event_cb;  // optional
    void *event_ctx;
} dc_mqtt_config_t;

// Allocate, initialize, and start a client. ESP-MQTT copies the configuration during
// initialization; the strings in config need only remain valid for this call.
esp_err_t dc_mqtt_start(const dc_mqtt_config_t *config, dc_mqtt_client_t **out_client);

// Stop, destroy, and free a client. The pointer is invalid after this call.
esp_err_t dc_mqtt_destroy(dc_mqtt_client_t *client);

// Return the ESP-MQTT message id, or -1 if the arguments/client are invalid.
int dc_mqtt_publish(dc_mqtt_client_t *client, const char *topic, const char *data,
                    int len, int qos, bool retain);
int dc_mqtt_subscribe(dc_mqtt_client_t *client, const char *topic, int qos);

#ifdef __cplusplus
}
#endif
