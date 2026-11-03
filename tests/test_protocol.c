#include "protocol.h"
#include "command_engine.h"
#include "journal.h"
#include "relay.h"
#include "fake_platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static kz_command parse(const char *s)
{
    size_t n = strlen(s); char *b = malloc(n + 1); assert(b); memcpy(b, s, n + 1);
    kz_command c = kz_parse(b, n); free(b); return c;
}
static kz_command with_state(const char *state)
{
    char b[2000]; snprintf(b, sizeof(b), "{\"command_id\":\"cmd\",\"correlation_id\":\"corr\","
        "\"timestamp\":\"2026-09-30T12:00:00.000Z\",\"state\":%s}", state);
    return parse(b);
}
static void parser_tests(void)
{
    kz_command c = with_state("{\"on\":true}");
    CHECK(c.error == KZ_OK && c.on && c.identifiable);
    CHECK(!with_state("{\"on\":false}").on);
    CHECK(with_state("{}").error == KZ_PROTOCOL);
    CHECK(with_state("null").error == KZ_PROTOCOL);
    CHECK(with_state("{\"on\":1}").error == KZ_INVALID);
    CHECK(with_state("{\"on\":null}").error == KZ_INVALID);
    CHECK(with_state("{\"on\":\"true\"}").error == KZ_INVALID);
    CHECK(with_state("{\"on\":true,\"brightness\":1}").error == KZ_UNSUPPORTED);
    CHECK(!with_state("{\"on\":true,\"on\":false}").identifiable);
    CHECK(!with_state("{\"on\":true,\"\\u006fn\":false}").identifiable);
    CHECK(!with_state("{\"on\":true,}").identifiable);
    CHECK(!with_state("{\"on\":01}").identifiable);
    CHECK(!with_state("{\"on\":NaN}").identifiable);
    CHECK(!with_state("{\"on\":1.}").identifiable);
    CHECK(!with_state("{\"on\":1e}").identifiable);
    CHECK(!parse("[]").identifiable);
    CHECK(!parse("{} garbage").identifiable);
    CHECK(!parse("{\"command_id\":\"x\"}").identifiable);
    CHECK(!parse("{\"command_id\":\"\\u0000x\",\"correlation_id\":\"a\"}").identifiable);
    const char *prefix = "{\"command_id\":\"cmd\",\"correlation_id\":\"corr\",\"timestamp\":\"2026-09-30T12:00:00Z\",\"state\":{\"on\":true}";
    char b[18000];
    snprintf(b, sizeof(b), "%s,\"optional\":{\"a\":[null,1,-2.5e+2,\"\\uD83D\\uDE00\"]}}", prefix);
    CHECK(parse(b).error == KZ_OK);
    snprintf(b, sizeof(b), "%s,\"protocol_version\":\"v2\"}", prefix);
    CHECK(parse(b).error == KZ_PROTOCOL);
    snprintf(b, sizeof(b), "%s,\"x\":\"\\uD800\"}", prefix); CHECK(!parse(b).identifiable);
    snprintf(b, sizeof(b), "%s,\"x\":\"\xc0\x80\"}", prefix); CHECK(!parse(b).identifiable);
    snprintf(b, sizeof(b), "%s,\"x\":[[[[[[[0]]]]]]]}", prefix); CHECK(parse(b).error == KZ_OK);
    snprintf(b, sizeof(b), "%s,\"x\":[[[[[[[[0]]]]]]]]}", prefix); CHECK(!parse(b).identifiable);
    memset(b, ' ', 16385); b[16385] = 0; CHECK(!parse(b).identifiable);
    snprintf(b, sizeof(b), "{\"command_id\":\"%0129d\",\"correlation_id\":\"c\"}", 0);
    CHECK(!parse(b).identifiable);
    CHECK(kz_topic_id("e2e_house-123"));
    CHECK(!kz_topic_id("../house")); CHECK(!kz_topic_id("a+b")); CHECK(!kz_topic_id("a#b")); CHECK(!kz_topic_id(""));
    char q[30]; CHECK(kz_quote(q, sizeof(q), "a\"\\\n") && !strcmp(q, "\"a\\\"\\\\\\u000a\""));
    CHECK(!kz_quote(q, 3, "long"));
    snprintf(b, sizeof(b), "%s,\"x\":\"%01024d\"}", prefix, 0);
    CHECK(parse(b).error == KZ_OK);
    snprintf(b, sizeof(b), "%s,\"x\":\"%01025d\"}", prefix, 0);
    CHECK(!parse(b).identifiable);
    snprintf(b, sizeof(b), "{\"command_id\":\"%0128d\",\"correlation_id\":\"corr\","
        "\"timestamp\":\"2026-09-30T12:00:00Z\",\"state\":{\"on\":false}}", 0);
    CHECK(parse(b).error == KZ_OK);
    snprintf(b, sizeof(b), "%s,\"extra\":[", prefix);
    for (int i = 0; i < 510; ++i) strcat(b, "0,");
    strcat(b, "0]}"); CHECK(!parse(b).identifiable);
    snprintf(b, sizeof(b), "%s}", prefix);
    size_t valid_size = strlen(b);
    memset(b + valid_size, ' ', KZ_PAYLOAD_MAX - valid_size);
    CHECK(kz_parse(b, KZ_PAYLOAD_MAX).error == KZ_OK);
    /* Deterministic malformed-input smoke fuzz exercises bounded rejection. */
    unsigned rng = 7;
    for (int i = 0; i < 20000; ++i) {
        size_t n = (unsigned)i % 511;
        for (size_t j = 0; j < n; ++j) { rng = rng * 1664525u + 1013904223u; b[j] = (char)(rng >> 24); }
        kz_command f = kz_parse(b, n); CHECK(!f.identifiable);
    }
}
static void time_tests(void)
{
    int64_t t, other;
    CHECK(kz_timestamp("2026-09-30T12:00:00.000Z", &t));
    CHECK(kz_timestamp("2026-09-30T12:00:00+00:00", &other) && other == t);
    CHECK(kz_timestamp("2026-09-30T12:00:00.123456Z", &other) && other == t + 123);
    CHECK(!kz_timestamp("2026-02-29T00:00:00Z", &other));
    CHECK(kz_timestamp("2024-02-29T00:00:00Z", &other));
    CHECK(!kz_timestamp("2026-09-30T24:00:00Z", &other));
    CHECK(!kz_timestamp("2026-09-30T00:00:60Z", &other));
    CHECK(!kz_timestamp("2026-09-30T00:00:00+03:00", &other));
    CHECK(kz_freshness(t, t + 300000) == KZ_OK);
    CHECK(kz_freshness(t, t + 300001) == KZ_TIMEOUT);
    CHECK(kz_freshness(t, t - 30000) == KZ_OK);
    CHECK(kz_freshness(t, t - 30001) == KZ_PROTOCOL);
    kz_record r = {.version=1, .first_ms=t, .timestamp_ms=t};
    CHECK(!kz_reclaimable(&r, t + 600000, 599999));
    CHECK(!kz_reclaimable(&r, t + 599999, 600000));
    CHECK(!kz_reclaimable(&r, t - 1, 700000));
    CHECK(kz_reclaimable(&r, t + 600000, 600000));
    r.timestamp_ms = t + 900000;
    CHECK(!kz_reclaimable(&r, t + 600000, 600000));
}
static void engine_tests(void)
{
    fake_reset_storage(); CHECK(journal_init() == ESP_OK);
    kz_command c = with_state("{\"on\":true}");
    for (int delay = 0; delay <= 300000; delay += 1000) {
        kz_result r = kz_process(&c, c.timestamp_ms + delay, false);
        CHECK(!r.ack && !r.report_state && fake_apply_count == 0);
    }
}
int main(void)
{
    parser_tests(); time_tests(); engine_tests();
    /* Regression: the former 60s late-execution gap has no legacy admission path. */
    fake_reset_storage(); CHECK(journal_init() == ESP_OK);
    kz_command late = with_state("{\"on\":true}");
    kz_result result = kz_process(&late, late.timestamp_ms + 60000, false);
    CHECK(!result.ack);
    CHECK(!relay_state() && fake_apply_count == 0);
    printf("PASS: %d checks (legacy parser, timestamps, reclaim rules, legacy actuation denied)\n", checks);
    return 0;
}
