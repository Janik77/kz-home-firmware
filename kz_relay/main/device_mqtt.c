#include "device_mqtt.h"
#include "config.h"
#include "protocol.h"
#include "journal.h"
#include "command_engine.h"
#include "relay.h"
#include "network.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_ESP_TLS_INSECURE) || defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY)
#error "KZ relay requires verified TLS"
#endif
#if !defined(CONFIG_MBEDTLS_HAVE_TIME_DATE)
#error "KZ relay requires certificate validity-date verification"
#endif

static const char *TAG = "kz_mqtt";
static esp_mqtt_client_handle_t client;
static char set_topic[288], state_topic[288], ack_topic[288], status_topic[288], client_id[140];
static QueueHandle_t incoming;
static atomic_bool ready, need_restart;
static atomic_uint generation;
static int subscribe_id;
static atomic_int subscribe_started;
static char *assembly;
static size_t received, expected;
static bool retained;
typedef struct { char *json; size_t size; bool retained; unsigned generation; } message;
/* Worker-owned reports: do not apply another command until both can be queued.
 * This retains the causal ID when a bounded MQTT outbox is temporarily full. */
static kz_record pending_ack;
static bool ack_pending, state_pending;
static char pending_correlation[129];

static bool publish(const char *topic, const char *payload, bool retain)
{
    /* enqueue never waits for a socket; bounded outbox refuses overload. */
    int id = esp_mqtt_client_enqueue(client, topic, payload, 0, 1, retain, true);
    if (id < 0) ESP_LOGW(TAG, "Publish not queued; state will be reconciled on heartbeat/reconnect");
    return id >= 0;
}

static bool state_report(const char *correlation)
{
    char stamp[25], quoted[775], payload[1100];
    if (!network_timestamp(stamp)) return false;
    if (correlation && !kz_quote(quoted, sizeof(quoted), correlation)) return false;
    if (correlation) snprintf(payload, sizeof(payload),
        "{\"timestamp\":\"%s\",\"correlation_id\":%s,\"state\":{\"on\":%s}}",
        stamp, quoted, relay_state() ? "true" : "false");
    else snprintf(payload, sizeof(payload), "{\"timestamp\":\"%s\",\"state\":{\"on\":%s}}",
        stamp, relay_state() ? "true" : "false");
    return publish(state_topic, payload, true);
}

static bool ack_report(const kz_record *r)
{
    char stamp[25], id[775], corr[775], payload[1900];
    if (!network_timestamp(stamp) || !kz_quote(id, sizeof(id), r->command_id) ||
        !kz_quote(corr, sizeof(corr), r->correlation_id)) return false;
    snprintf(payload, sizeof(payload),
        "{\"command_id\":%s,\"correlation_id\":%s,\"timestamp\":\"%s\",\"status\":\"%s\"%s%s%s}",
        id, corr, stamp, kz_outcome_name(r->outcome),
        r->error ? ",\"error_code\":\"" : "", r->error ? kz_error_name(r->error) : "", r->error ? "\"" : "");
    return publish(ack_topic, payload, false);
}

static bool online_report(void)
{
    char stamp[25], payload[300];
    if (!network_timestamp(stamp)) return false;
    snprintf(payload, sizeof(payload), "{\"timestamp\":\"%s\",\"status\":\"online\","
        "\"last_seen\":\"%s\",\"heartbeat_interval_seconds\":60,\"protocol_version\":\"v1\","
        "\"firmware_version\":\"%s\",\"hardware_model\":\"%s\"}",
        stamp, stamp, KZ_FIRMWARE_VERSION, KZ_HARDWARE_MODEL);
    return publish(status_topic, payload, true);
}

static bool drain_reports(void)
{
    if (ack_pending) {
        if (!ack_report(&pending_ack)) return false;
        ack_pending = false;
    }
    if (state_pending) {
        if (!state_report(pending_correlation)) return false;
        state_pending = false;
    }
    return true;
}

static void command(message *m)
{
    kz_command c = kz_parse(m->json, m->size);
    if (!c.identifiable) { ESP_LOGW(TAG, "Invalid JSON/envelope; command dropped without GPIO change"); return; }
    kz_result result = kz_process(&c, network_now_ms(), m->retained);
    if (result.ack) {
        pending_ack = *result.ack; ack_pending = true;
        ESP_LOGI(TAG, "Command outcome: %s", kz_outcome_name(result.ack->outcome));
    } else ESP_LOGW(TAG, "No durable ACK: storage unavailable/full or conflicting command ID");
    if (result.report_state) {
        memcpy(pending_correlation, c.correlation_id, sizeof(pending_correlation));
        state_pending = true;
    }
    drain_reports();
}

static void reset_assembly(void)
{
    free(assembly); assembly = NULL; expected = received = 0;
}

static void data_event(esp_mqtt_event_handle_t e)
{
    if (e->current_data_offset == 0) {
        reset_assembly();
        if (!atomic_load(&ready) || e->topic_len != (int)strlen(set_topic) ||
            !e->topic || memcmp(e->topic, set_topic, e->topic_len) != 0 ||
            e->total_data_len <= 0 || e->total_data_len > KZ_PAYLOAD_MAX) return;
        expected = (size_t)e->total_data_len; retained = e->retain;
        assembly = malloc(expected + 1);
        if (!assembly) { expected = 0; return; }
    }
    if (!assembly) return;
    if (e->current_data_offset < 0 || (size_t)e->current_data_offset != received ||
        e->total_data_len != (int)expected || e->data_len <= 0 || !e->data ||
        (size_t)e->data_len > expected - received) { reset_assembly(); return; }
    memcpy(assembly + received, e->data, e->data_len); received += e->data_len;
    if (received == expected) {
        assembly[expected] = 0;
        message m = {assembly, expected, retained, atomic_load(&generation)};
        if (xQueueSend(incoming, &m, 0) != pdTRUE) free(assembly);
        assembly = NULL; expected = received = 0;
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        atomic_store(&ready, false); atomic_fetch_add(&generation, 1); reset_assembly();
        atomic_store(&subscribe_started, (int)(esp_timer_get_time() / 1000000));
        subscribe_id = esp_mqtt_client_subscribe(client, set_topic, 1);
        if (subscribe_id < 0) atomic_store(&need_restart, true);
        ESP_LOGI(TAG, "TLS MQTT connected; waiting for set subscription");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (e->msg_id == subscribe_id) {
            /* v3.1.1 SUBACK payload contains one granted QoS for our one topic.
             * ESP-MQTT sets error_handle on SUBSCRIBED even for a failed grant. */
            if (e->data_len == 1 && e->data && (unsigned char)e->data[0] == 1 &&
                (!e->error_handle || e->error_handle->error_type == MQTT_ERROR_TYPE_NONE)) {
                atomic_store(&ready, true);
                ESP_LOGI(TAG, "Set subscription acknowledged at QoS 1");
            } else atomic_store(&need_restart, true);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        atomic_store(&ready, false); atomic_fetch_add(&generation, 1);
        atomic_store(&subscribe_started, 0); reset_assembly();
        ESP_LOGW(TAG, "MQTT disconnected; automatic reconnect enabled");
        break;
    case MQTT_EVENT_DATA: data_event(e); break;
    case MQTT_EVENT_ERROR:
        atomic_store(&ready, false); atomic_store(&need_restart, true);
        ESP_LOGW(TAG, "MQTT/TLS error; details and credentials suppressed");
        break;
    default: break;
    }
}

static void worker(void *arg)
{
    (void)arg;
    int64_t heartbeat = 0, retry_at = 0, offline_retry = 0;
    unsigned announced = 0;
    bool clock_stopped = false;
    for (;;) {
        message m;
        bool got = xQueueReceive(incoming, &m, pdMS_TO_TICKS(250)) == pdTRUE;
        int64_t now = network_now_ms(), mono = esp_timer_get_time();
        if (!now && !clock_stopped && mono >= offline_retry) {
            atomic_store(&ready, false);
            /* Exceptional shutdown only: send on the socket before DISCONNECT,
             * preserving TCP ordering. Ordinary publications use enqueue. */
            int sent = esp_mqtt_client_publish(client, status_topic,
                "{\"status\":\"offline\"}", 0, 1, true);
            if (sent >= 0) {
                esp_mqtt_client_stop(client); clock_stopped = true;
            }
            offline_retry = mono + 10000000;
        }
        if (now && !clock_stopped && atomic_load(&ready) && network_has_ip()) {
            unsigned gen = atomic_load(&generation);
            bool reports_clear = drain_reports();
            if (announced != gen || mono >= heartbeat) {
                if ((announced == gen || state_report(NULL)) && online_report()) {
                    announced = gen; heartbeat = mono + 60000000;
                } else heartbeat = mono + 1000000;
            }
            if (got && reports_clear && m.generation == gen) command(&m);
        }
        if (got) free(m.json);
        int sub_start = atomic_load(&subscribe_started);
        if (sub_start && !atomic_load(&ready) && mono / 1000000 - sub_start > 15)
            atomic_store(&need_restart, true);
        if (now && !clock_stopped && mono >= retry_at && atomic_exchange(&need_restart, false)) {
            /* Handles failed SUBACK/start/subscribe as well as transport errors. */
            esp_mqtt_client_stop(client);
            if (esp_mqtt_client_start(client) != ESP_OK) atomic_store(&need_restart, true);
            retry_at = mono + 10000000;
        }
    }
}

esp_err_t device_mqtt_start(void)
{
    snprintf(set_topic, sizeof(set_topic), "kzhome/v1/%s/%s/set", KZ_HOUSE_ID, KZ_DEVICE_ID);
    snprintf(state_topic, sizeof(state_topic), "kzhome/v1/%s/%s/state", KZ_HOUSE_ID, KZ_DEVICE_ID);
    snprintf(ack_topic, sizeof(ack_topic), "kzhome/v1/%s/%s/ack", KZ_HOUSE_ID, KZ_DEVICE_ID);
    snprintf(status_topic, sizeof(status_topic), "kzhome/v1/%s/%s/status", KZ_HOUSE_ID, KZ_DEVICE_ID);
    snprintf(client_id, sizeof(client_id), "kz-%s", KZ_DEVICE_ID);
    incoming = xQueueCreate(4, sizeof(message));
    if (!incoming) return ESP_ERR_NO_MEM;
    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = KZ_MQTT_HOST,
        .broker.address.port = KZ_MQTT_PORT,
        .broker.address.transport = MQTT_TRANSPORT_OVER_SSL,
        .broker.verification.certificate = kz_broker_ca_start,
        .broker.verification.skip_cert_common_name_check = false,
        .credentials.client_id = client_id,
        .credentials.username = KZ_MQTT_USERNAME,
        .credentials.authentication.password = KZ_MQTT_PASSWORD,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .session.disable_clean_session = false,
        .session.keepalive = 30,
        .session.last_will = {.topic = status_topic, .msg = "{\"status\":\"offline\"}", .qos = 1, .retain = true},
        .network.disable_auto_reconnect = false,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
        .buffer.size = 2048,
        .outbox.limit = 32768,
    };
    client = esp_mqtt_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    if (err != ESP_OK) return err;
    /* Parser uses bounded stack space; callbacks only reassemble and enqueue. */
    if (xTaskCreate(worker, "kz_device", 24576, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    err = esp_mqtt_client_start(client);
    if (err != ESP_OK) atomic_store(&need_restart, true);
    return ESP_OK;
}
