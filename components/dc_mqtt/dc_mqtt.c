#include "dc_mqtt.h"

#include "mqtt_client.h"

#include <stdlib.h>
#include <string.h>

struct dc_mqtt_client {
    esp_mqtt_client_handle_t handle;
    dc_mqtt_event_cb_t event_cb;
    void *event_ctx;
};

static void mqtt_event_handler(void *args, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)event_base;
    dc_mqtt_client_t *client = (dc_mqtt_client_t *)args;
    if (client == NULL || client->event_cb == NULL) return;

    esp_mqtt_event_handle_t raw = (esp_mqtt_event_handle_t)event_data;
    dc_mqtt_event_t event = {
        .client = client,
    };

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        event.type = DC_MQTT_EVENT_CONNECTED;
        break;
    case MQTT_EVENT_DISCONNECTED:
        event.type = DC_MQTT_EVENT_DISCONNECTED;
        break;
    case MQTT_EVENT_DATA:
        event.type = DC_MQTT_EVENT_MESSAGE;
        event.topic = raw->topic;
        event.topic_len = raw->topic_len;
        event.data = raw->data;
        event.data_len = raw->data_len;
        event.total_data_len = raw->total_data_len;
        event.current_data_offset = raw->current_data_offset;
        break;
    case MQTT_EVENT_ERROR:
        event.type = DC_MQTT_EVENT_ERROR;
        break;
    default:
        return;
    }

    client->event_cb(client->event_ctx, &event);
}

esp_err_t dc_mqtt_start(const dc_mqtt_config_t *config, dc_mqtt_client_t **out_client)
{
    if (config == NULL || out_client == NULL || config->broker_uri == NULL ||
        config->broker_uri[0] == '\0') return ESP_ERR_INVALID_ARG;
    *out_client = NULL;

    dc_mqtt_client_t *client = calloc(1, sizeof(*client));
    if (client == NULL) return ESP_ERR_NO_MEM;
    client->event_cb = config->event_cb;
    client->event_ctx = config->event_ctx;

    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = config->broker_uri,
        .broker.verification.skip_cert_common_name_check = config->skip_cert_common_name_check,
    };
    if (config->username && config->username[0])
        mqtt_config.credentials.username = config->username;
    if (config->password && config->password[0])
        mqtt_config.credentials.authentication.password = config->password;
    if (config->last_will_topic) {
        mqtt_config.session.last_will.topic = config->last_will_topic;
        mqtt_config.session.last_will.msg = config->last_will_message
            ? config->last_will_message : "";
        mqtt_config.session.last_will.msg_len = config->last_will_message
            ? (int)strlen(config->last_will_message) : 0;
        mqtt_config.session.last_will.qos = config->last_will_qos;
        mqtt_config.session.last_will.retain = config->last_will_retain;
    }

    client->handle = esp_mqtt_client_init(&mqtt_config);
    if (client->handle == NULL) {
        free(client);
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(
        client->handle, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    if (err == ESP_OK) {
        // Publish the wrapper to the caller before the MQTT task can issue a fast
        // CONNECTED callback. Product callbacks commonly publish/subscribe through
        // their stored client pointer, matching the ordering of raw ESP-MQTT setup.
        *out_client = client;
        err = esp_mqtt_client_start(client->handle);
    }
    if (err != ESP_OK) {
        *out_client = NULL;
        esp_mqtt_client_destroy(client->handle);
        free(client);
        return err;
    }
    return ESP_OK;
}

esp_err_t dc_mqtt_destroy(dc_mqtt_client_t *client)
{
    if (client == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t stop_err = esp_mqtt_client_stop(client->handle);
    esp_err_t destroy_err = esp_mqtt_client_destroy(client->handle);
    free(client);
    return stop_err != ESP_OK ? stop_err : destroy_err;
}

int dc_mqtt_publish(dc_mqtt_client_t *client, const char *topic, const char *data,
                    int len, int qos, bool retain)
{
    if (client == NULL || client->handle == NULL || topic == NULL) return -1;
    return esp_mqtt_client_publish(client->handle, topic, data, len, qos, retain);
}

int dc_mqtt_subscribe(dc_mqtt_client_t *client, const char *topic, int qos)
{
    if (client == NULL || client->handle == NULL || topic == NULL) return -1;
    return esp_mqtt_client_subscribe(client->handle, topic, qos);
}
