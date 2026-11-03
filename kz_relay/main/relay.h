#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "protocol.h"
esp_err_t relay_init(void);
bool relay_apply(bool on);
bool relay_state(void);
bool relay_apply_permitted(bool on, const kz_budget *budget);
