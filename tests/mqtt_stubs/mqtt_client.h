#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef const char *esp_event_base_t;
typedef void *esp_mqtt_client_handle_t;
enum { MQTT_EVENT_CONNECTED, MQTT_EVENT_SUBSCRIBED, MQTT_EVENT_DISCONNECTED,
       MQTT_EVENT_DATA, MQTT_EVENT_ERROR, ESP_EVENT_ANY_ID };
enum { MQTT_ERROR_TYPE_NONE, MQTT_TRANSPORT_OVER_SSL = 2, MQTT_PROTOCOL_V_3_1_1 = 3 };
typedef struct { int error_type; } mqtt_error;
typedef struct {
    int current_data_offset, total_data_len, topic_len, data_len, msg_id;
    char *topic, *data;
    bool retain;
    mqtt_error *error_handle;
} esp_mqtt_event_t, *esp_mqtt_event_handle_t;
typedef struct {
    struct {
        struct { const char *hostname; int port, transport; } address;
        struct { const char *certificate; bool skip_cert_common_name_check; } verification;
    } broker;
    struct {
        const char *client_id, *username;
        struct { const char *password; } authentication;
    } credentials;
    struct {
        int protocol_ver, keepalive;
        bool disable_clean_session;
        struct { const char *topic, *msg; int qos; bool retain; } last_will;
    } session;
    struct { bool disable_auto_reconnect; int reconnect_timeout_ms, timeout_ms; } network;
    struct { int size; } buffer;
    struct { int limit; } outbox;
} esp_mqtt_client_config_t;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, int event,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client, const char *topic,
    const char *data, int len, int qos, int retain, bool store);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t client, const char *topic,
    const char *data, int len, int qos, int retain);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client);
