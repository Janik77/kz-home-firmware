#pragma once
#include "protocol.h"
#include "esp_err.h"
esp_err_t journal_init(void);
const kz_record *journal_find(const char *command_id);
int journal_reserve(const kz_command *cmd, int64_t now_ms, kz_error error);
bool journal_finish(int slot, bool applied);
const kz_record *journal_get(int slot);
bool journal_healthy(void);
int64_t journal_clock_floor(void);
