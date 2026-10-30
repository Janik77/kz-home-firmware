#include "relay.h"
#include "board.h"
#include "driver/gpio.h"
static bool current_on;
static bool initialized;
esp_err_t relay_init(void)
{
    initialized = false;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(KZ_RELAY_GPIO)) return ESP_ERR_INVALID_ARG;
    /* Preload inactive level before enabling output. External bias is still
     * required to establish OFF before application startup and during reset. */
    esp_err_t err = gpio_set_level(KZ_RELAY_GPIO, KZ_RELAY_OFF_LEVEL);
    if (err != ESP_OK) return err;
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << KZ_RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    err = gpio_config(&cfg);
    if (err == ESP_OK) { current_on = false; initialized = true; }
    return err;
}
bool relay_apply(bool on)
{
    if (!initialized || gpio_set_level(KZ_RELAY_GPIO,
            on ? KZ_RELAY_ON_LEVEL : KZ_RELAY_OFF_LEVEL) != ESP_OK) return false;
    current_on = on; return true;
}
bool relay_state(void) { return current_on; }
