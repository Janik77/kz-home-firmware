#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "relay.h"
#include "driver/gpio.h"

static int calls, set_calls, config_calls;
static unsigned last_level;
static bool fail_set, fail_config;
esp_err_t gpio_set_level(int pin, unsigned level)
{
    assert(pin == KZ_RELAY_GPIO);
    ++calls; ++set_calls; last_level = level;
    return fail_set ? ESP_FAIL : ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t *cfg)
{
    /* The very first hardware call must preload OFF, before output enable. */
    assert(calls == 1 && last_level == KZ_RELAY_OFF_LEVEL);
    assert(cfg->pin_bit_mask == (1ULL << KZ_RELAY_GPIO));
    assert(cfg->mode == GPIO_MODE_OUTPUT);
    assert(cfg->pull_up_en == GPIO_PULLUP_DISABLE);
    assert(cfg->pull_down_en == GPIO_PULLDOWN_DISABLE);
    assert(cfg->intr_type == GPIO_INTR_DISABLE);
    ++calls; ++config_calls;
    return fail_config ? ESP_FAIL : ESP_OK;
}
static void reset_calls(void) { calls = set_calls = config_calls = 0; }
int main(void)
{
#ifdef KZ_BOARD_esp32_wroom
    assert(strcmp(KZ_HARDWARE_MODEL, "kz-relay-esp32-wroom") == 0);
#else
    assert(strcmp(KZ_HARDWARE_MODEL, "kz-relay-esp32c6") == 0);
#endif
    assert(!relay_apply(true) && calls == 0);
    assert(relay_init() == ESP_OK && !relay_state());
    assert(set_calls == 1 && config_calls == 1 && last_level == KZ_RELAY_OFF_LEVEL);
    assert(relay_apply(true) && relay_state() && last_level == KZ_RELAY_ON_LEVEL);
    fail_set = true;
    assert(!relay_apply(false) && relay_state());
    fail_set = false;
    assert(relay_apply(false) && !relay_state() && last_level == KZ_RELAY_OFF_LEVEL);
    reset_calls(); fail_set = true;
    assert(relay_init() == ESP_FAIL && config_calls == 0);
    fail_set = false;
    assert(!relay_apply(true));
    reset_calls(); fail_config = true;
    assert(relay_init() == ESP_FAIL && config_calls == 1);
    fail_config = false;
    assert(!relay_apply(true));
    reset_calls();
    assert(relay_init() == ESP_OK && !relay_state());
    printf("PASS: %s GPIO %d active-high=%d initialization/polarity/faults\n",
           KZ_HARDWARE_MODEL, KZ_RELAY_GPIO, KZ_RELAY_ACTIVE_HIGH);
    return 0;
}
