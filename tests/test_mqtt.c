/* Exercise the real MQTT callback/worker with deterministic SDK boundaries.
 * Including the implementation exposes static functions without firmware hooks. */
#include <assert.h>
#include <setjmp.h>
#include "fake_platform.h"
#include "../kz_relay/main/device_mqtt.c"

const char kz_broker_ca_start[] = "synthetic-test-ca";
static int64_t now = 1793361600000LL;
static const char *stamp = "2026-10-30T12:00:00.000Z";
static bool bad_clock, fail_publish;
static int init_calls, starts, stops, publishes, queued, status_count, receive_count;
static int iterations;
static int64_t step_us;
static jmp_buf worker_exit;
static void (*iteration_hook)(int);
/* Optional host integration observer; no change to the production worker. */
static void (*publication_hook)(const char *, const char *, int, int);
static esp_mqtt_client_config_t captured;
static message queue[4];
static unsigned queue_size;
static char last_payload[1200], startup_will[128];

bool network_timestamp(char out[25])
{
    if (bad_clock) return false;
    snprintf(out, 25, "%s", stamp); return true;
}
int64_t network_now_ms(void) { return bad_clock ? 0 : now; }
bool network_has_ip(void) { return true; }
QueueHandle_t xQueueCreate(unsigned count, unsigned size)
{ assert(count == 4 && size == sizeof(message)); return queue; }
int xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)q; (void)wait;
    if (queue_size == 4) return pdFALSE;
    queue[queue_size++] = *(const message *)item; return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)q; (void)wait;
    if (++receive_count > iterations) longjmp(worker_exit, 1);
    if (iteration_hook) iteration_hook(receive_count);
    fake_mono_ms += step_us / 1000;
    if (!queue_size) return pdFALSE;
    *(message *)item = queue[0];
    memmove(queue, queue + 1, --queue_size * sizeof(message)); return pdTRUE;
}
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    assert(task == worker); return pdPASS;
}
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg)
{
    ++init_calls; captured = *cfg;
    assert(cfg->broker.address.transport == MQTT_TRANSPORT_OVER_SSL);
    assert(!cfg->broker.verification.skip_cert_common_name_check);
    assert(cfg->broker.verification.certificate == kz_broker_ca_start);
    assert(strcmp(cfg->credentials.client_id, "kz-test_relay") == 0);
    assert(strcmp(cfg->session.last_will.topic, "kzhome/v1/test_house/test_relay/status") == 0);
    assert(cfg->session.last_will.qos == 1 && cfg->session.last_will.retain);
    snprintf(startup_will, sizeof(startup_will), "%s", cfg->session.last_will.msg);
    printf("LWT\t%s\n", startup_will);
    return &captured;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, int event,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg)
{
    (void)c; (void)arg; assert(event == ESP_EVENT_ANY_ID && handler == mqtt_event); return ESP_OK;
}
static void record(const char *topic, const char *data, int qos, int retain, bool sync)
{
    assert(qos == 1);
    if (publication_hook) publication_hook(topic, data, qos, retain);
    snprintf(last_payload, sizeof(last_payload), "%s", data);
    if (strcmp(topic, status_topic) == 0) {
        assert(retain); ++status_count;
        printf("%s\t%s\n", sync ? "OFFLINE" : "ONLINE", data);
    }
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t c, const char *topic,
    const char *data, int len, int qos, int retain, bool store)
{
    (void)c; assert(len == 0 && store); ++queued;
    record(topic, data, qos, retain, false); return queued;
}
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic,
    const char *data, int len, int qos, int retain)
{
    (void)c; assert(len == 0); ++publishes;
    record(topic, data, qos, retain, true); return fail_publish ? -1 : publishes;
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char *topic, int qos)
{ (void)c; assert(qos == 1); if (!strcmp(topic, command_topic)) return 42; assert(!strcmp(topic, reply_topic)); return 43; }
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c)
{ (void)c; ++starts; return ESP_OK; }
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c)
{
    (void)c; ++stops;
    assert(strstr(last_payload, "\"status\":\"offline\""));
    assert(strstr(last_payload, "\"last_seen\":\""));
    assert(!strstr(last_payload, "\"timestamp\""));
    return ESP_OK;
}
static void event(int id, int grant)
{
    char qos = (char)grant;
    esp_mqtt_event_t e = {.msg_id = 42, .data = &qos, .data_len = 1};
    mqtt_event(NULL, NULL, id, &e);
    if (id == MQTT_EVENT_SUBSCRIBED) { e.msg_id = 43; mqtt_event(NULL, NULL, id, &e); }
}
static void tick(int count, int64_t advance_us)
{
    receive_count = 0; iterations = count; step_us = advance_us;
    if (!setjmp(worker_exit)) worker(NULL);
}
static void recover_publish(int iteration)
{ if (iteration == 3) fail_publish = false; }
static void heartbeat_clock(int iteration)
{
    if (iteration == 3) { stamp = "2026-10-30T12:01:00.000Z"; now += 60000; }
}
static void inject(const char *topic, const char *data)
{
    esp_mqtt_event_t e = {.topic = (char *)topic, .topic_len = (int)strlen(topic),
        .data = (char *)data, .data_len = (int)strlen(data), .total_data_len = (int)strlen(data)};
    mqtt_event(NULL, NULL, MQTT_EVENT_DATA, &e);
}
int main(void)
{
    fake_reset_storage(); assert(journal_init() == ESP_OK);
    fake_mono_ms = 1000;
    bad_clock = true;
    assert(device_mqtt_start() == ESP_ERR_INVALID_STATE && init_calls == 0);
    bad_clock = false; stamp = "invalid";
    assert(device_mqtt_start() == ESP_ERR_INVALID_STATE && init_calls == 0);
    stamp = "2026-10-30T12:00:00.000Z";
    assert(device_mqtt_start() == ESP_OK && starts == 1);
    event(MQTT_EVENT_CONNECTED, 0);
    tick(1, 0); assert(status_count == 0 && !atomic_load(&ready));
    event(MQTT_EVENT_SUBSCRIBED, 0); /* QoS 0 must not permit online. */
    tick(1, 0); assert(stops == 1 && starts == 2);
    event(MQTT_EVENT_CONNECTED, 0); event(MQTT_EVENT_SUBSCRIBED, 1);
    int before = status_count;
    iteration_hook = heartbeat_clock;
    tick(3, 30000000); assert(status_count == before + 2); /* Initial + 60s heartbeat. */
    iteration_hook = NULL;
    assert(strstr(last_payload, "\"status\":\"online\""));
    assert(strstr(last_payload, "\"heartbeat_interval_seconds\":60"));
    assert(strstr(last_payload, "12:01:00.000Z"));
    assert(strcmp(captured.session.last_will.msg, startup_will) == 0);

    /* Full worker exchange stays blocked by the unqualified production policy. */
    const char *candidate = "{\"protocol_version\":\"v1\",\"execution_profile\":\"deadline-permit-v1\","
        "\"core_epoch\":\"11111111-1111-4111-8111-111111111111\","
        "\"command_id\":\"22222222-2222-4222-8222-222222222222\","
        "\"correlation_id\":\"33333333-3333-4333-8333-333333333333\","
        "\"timestamp\":\"2026-11-02T10:00:00Z\",\"expires_at\":\"2026-11-02T10:00:30Z\",\"state\":{\"on\":true}}";
    inject("kzhome/v1/test_house/test_relay/set", candidate); assert(queue_size == 0);
    inject("kzhome/v1/other_house/test_relay/control/command", candidate); assert(queue_size == 0);
    esp_mqtt_event_t retained = {.topic=command_topic,.topic_len=(int)strlen(command_topic),
        .data=(char *)candidate,.data_len=(int)strlen(candidate),
        .total_data_len=(int)strlen(candidate),.retain=1};
    mqtt_event(NULL,NULL,MQTT_EVENT_DATA,&retained); assert(queue_size==0);
    inject(command_topic, candidate); tick(1, 0);
    assert(control.waiting && strstr(last_payload,"permit_request") && !fake_apply_count);
    char grant[1400];
    snprintf(grant,sizeof(grant),"{\"control_version\":1,\"type\":\"permit_grant\","
        "\"execution_profile\":\"deadline-permit-v1\",\"core_epoch\":\"%s\",\"command_id\":\"%s\","
        "\"boot_id\":\"%s\",\"session_id\":\"%s\",\"nonce\":\"%s\",\"intent_sha256\":\"%s\","
        "\"core_time\":\"2026-11-02T10:00:01Z\",\"expires_at\":\"2026-11-02T10:00:30Z\",\"remaining_us\":1000000}",
        control.command.epoch,control.command.command_id,control.boot,control.session,control.nonce,control.command.digest);
    inject(reply_topic,grant); tick(1,0);
    assert(!control.waiting && !fake_apply_count && strstr(last_payload,"rejected"));
    inject(reply_topic,grant); tick(1,0); assert(!fake_apply_count);

    /* Status-topic and malformed command traffic cannot actuate the relay. */
    inject(status_topic, "{\"status\":\"online\"}"); assert(queue_size == 0);
    inject(command_topic, "{\"status\":\"offline\"}"); tick(1, 0);
    assert(fake_apply_count == 0);
    stamp = "malformed"; before = status_count;
    assert(!online_report() && status_count == before);
    stamp = "2026-10-30T12:01:00.000Z";
    event(MQTT_EVENT_DISCONNECTED, 0); before = status_count;
    tick(1, 0); assert(status_count == before && !atomic_load(&ready));
    event(MQTT_EVENT_CONNECTED, 0);
    tick(1, 0); assert(status_count == before); /* Wait for SUBACK again. */
    event(MQTT_EVENT_SUBSCRIBED, 1); tick(1, 0);
    assert(status_count == before + 1 && strstr(last_payload, "12:01:00.000Z"));
    assert(strcmp(captured.session.last_will.msg, startup_will) == 0);
    /* Recovery is a graceful clean stop: valid offline must precede stop. */
    event(MQTT_EVENT_ERROR, 0); int prior_stops = stops;
    tick(1, 0); assert(stops == prior_stops + 1 && !atomic_load(&ready));
    /* Clock fault uses cached time, retries a failed send and never executes. */
    bad_clock = true; fail_publish = true; prior_stops = stops;
    iteration_hook = recover_publish;
    tick(3, 5000000);
    assert(stops == prior_stops + 1 && strstr(last_payload, "12:01:00.000Z"));
    assert(fake_apply_count == 0 && !atomic_load(&ready));
    puts("PASS: MQTT TLS config, LWT, SUBACK, heartbeat, disconnect/reconnect, clean stop and clock-fault retry");
    return 0;
}
