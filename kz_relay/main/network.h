#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
bool network_config_valid(void);
esp_err_t network_start(void);
bool network_has_ip(void);
/* 0 means unavailable/untrusted: do not process commands or advertise online. */
int64_t network_now_ms(void);
bool network_timestamp(char output[25]);
