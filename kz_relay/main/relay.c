#include "relay.h"
#include "driver/gpio.h"
#define RELAY_GPIO GPIO_NUM_2
static bool current_on;
esp_err_t relay_init(void)
{
    /* Existing polarity: GPIO LOW = OFF, HIGH = ON. */
    esp_err_t err = gpio_set_level(RELAY_GPIO, 0);
    if (err != ESP_OK) return err;
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    err = gpio_config(&cfg);
    if (err == ESP_OK) current_on = false;
    return err;
}
bool relay_apply(bool on)
{
    if (gpio_set_level(RELAY_GPIO, on ? 1 : 0) != ESP_OK) return false;
    current_on = on; return true;
}
bool relay_state(void) { return current_on; }
