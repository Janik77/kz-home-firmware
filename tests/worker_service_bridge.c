/* Reuse the existing SDK/queue harness and actual device_mqtt.c worker.
 * Python carries publications over the real broker; it never creates ACK/state. */
#include <stdio.h>
#include <stdarg.h>
static int quiet_printf(const char *format, ...) { (void)format; return 0; }
#define printf quiet_printf
#define main mqtt_unit_test_main
#include "test_mqtt.c"
#undef main
#undef printf

static void publication(const char *topic, const char *payload, int qos, int retain)
{
    printf("PUB\t%s\t%d\t%d\t%s\n", topic, qos, retain, payload);
}

int main(void)
{
    char line[8200];
    fake_reset_storage(); assert(journal_init() == ESP_OK);
    fake_mono_ms = 1000;
    publication_hook = publication;
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        char *payload = strchr(line, '\t'); if (payload) *payload++ = 0;
        if (!strcmp(line, "start")) {
            assert(device_mqtt_start() == ESP_OK);
            /* The Python side first completes real CONNECT and both SUBACKs. */
            event(MQTT_EVENT_CONNECTED, 0); event(MQTT_EVENT_SUBSCRIBED, 1); tick(1, 0);
        } else if (!strcmp(line, "advance") && payload) {
            fake_mono_ms += strtoll(payload, NULL, 10);
        } else if (!strcmp(line, "candidate") && payload) {
            inject(command_topic, payload); tick(1, 0);
        } else if (!strcmp(line, "reply") && payload) {
            inject(reply_topic, payload); tick(1, 0);
        } else { assert(!"unsupported host operation"); }
        printf("{\"writes\":%d,\"waiting\":%s,\"on\":%s}\n", fake_apply_count,
               control.waiting ? "true" : "false", relay_state() ? "true" : "false");
        fflush(stdout);
    }
    return 0;
}
