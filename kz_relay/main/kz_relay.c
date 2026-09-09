#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define RELAY_GPIO GPIO_NUM_2

static const char *TAG = "kz_relay";

void app_main(void)
{
    gpio_config_t relay_config = {
        .pin_bit_mask = 1ULL << RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_set_level(RELAY_GPIO, 0);
    gpio_config(&relay_config);

    bool relay_on = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        relay_on = !relay_on;
        gpio_set_level(RELAY_GPIO, relay_on);
        ESP_LOGI(TAG, "Relay %s", relay_on ? "ON" : "OFF");
    }
}
