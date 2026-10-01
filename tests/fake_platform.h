#pragma once
#include <stdbool.h>
#include <stdint.h>
extern int fake_apply_count, fake_fail_commit;
extern int64_t fake_mono_ms;
extern bool fake_gpio_fails;
void fake_reset_storage(void);
void fake_reboot(void);
