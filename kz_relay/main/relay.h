#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t relay_init(void);
bool relay_apply(bool on);
bool relay_state(void);
