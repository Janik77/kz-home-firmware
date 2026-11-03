#pragma once
#include "protocol.h"

typedef struct {
    kz_command command;
    char boot[33], session[33], nonce[33];
    int64_t start_us, received_wall_ms;
    unsigned generation;
    bool waiting;
    const kz_record *duplicate;
} kz_control;

void control_init(kz_control *state);
void control_session(kz_control *state, unsigned generation, int64_t wall_ms);
void control_cancel(kz_control *state, int64_t wall_ms);
bool control_candidate(kz_control *state, char *raw, size_t n, unsigned generation,
                       int64_t wall_ms, int64_t mono_us);
bool control_request(const kz_control *state, char *out, size_t size);
bool control_reply(kz_control *state, char *raw, size_t n, unsigned generation,
                   int64_t mono_us, kz_budget *budget);
bool control_hash(const char *raw, size_t size, char out[65]);
