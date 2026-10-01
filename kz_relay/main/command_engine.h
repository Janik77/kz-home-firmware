#pragma once
#include "protocol.h"
typedef struct {
    const kz_record *ack;
    bool report_state;
} kz_result;
/* Single worker only. Durable intent -> GPIO -> durable result -> caller ACK. */
kz_result kz_process(const kz_command *command, int64_t now_ms, bool retained);
