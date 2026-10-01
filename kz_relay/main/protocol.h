#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KZ_PAYLOAD_MAX 16384
#define KZ_ID_MAX 128
#define KZ_AGE_MS 300000LL
#define KZ_FUTURE_MS 30000LL
#define KZ_DEDUP_MS 600000LL
#define KZ_JOURNAL_SLOTS 128

typedef enum { KZ_OK, KZ_PROTOCOL, KZ_UNSUPPORTED, KZ_INVALID,
               KZ_TIMEOUT, KZ_HARDWARE } kz_error;
typedef enum { KZ_PENDING = 1, KZ_APPLIED, KZ_REJECTED, KZ_FAILED } kz_outcome;
typedef struct {
    char command_id[129], correlation_id[129];
    int64_t timestamp_ms;
    bool on, identifiable;
    kz_error error;
} kz_command;
typedef struct {
    uint32_t version;
    uint32_t outcome, error;
    int64_t first_ms, timestamp_ms;
    char command_id[129], correlation_id[129];
    uint8_t on;
} kz_record;

/* Mutates the input buffer. No heap allocation, network, GPIO or IDF dependency. */
kz_command kz_parse(char *json, size_t length);
bool kz_timestamp(const char *text, int64_t *ms);
kz_error kz_freshness(int64_t command_ms, int64_t now_ms);
bool kz_topic_id(const char *id);
bool kz_reclaimable(const kz_record *r, int64_t now_ms, int64_t resident_ms);
bool kz_same_command(const kz_record *r, const kz_command *c);
const char *kz_error_name(kz_error error);
const char *kz_outcome_name(kz_outcome outcome);
/* JSON string escaping; returns 0 on insufficient capacity. */
size_t kz_quote(char *out, size_t capacity, const char *text);
