#include "relay.h"
#include "network.h"
#include "journal.h"
#include "device_mqtt.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    if (relay_init() != ESP_OK) { ESP_LOGE("kz", "Relay initialization failed"); return; }
    if (!network_config_valid()) { ESP_LOGE("kz", "Local configuration missing/invalid; relay remains OFF"); return; }
    /* Do not erase NVS on an error: that would destroy replay protection. */
    if (nvs_flash_init() != ESP_OK || journal_init() != ESP_OK) {
        ESP_LOGE("kz", "Persistent storage unavailable; relay remains OFF"); return;
    }
    if (network_start() != ESP_OK) { ESP_LOGE("kz", "Network initialization failed"); return; }
    while (!network_has_ip() || !network_now_ms()) vTaskDelay(pdMS_TO_TICKS(500));
    if (device_mqtt_start() != ESP_OK) ESP_LOGE("kz", "MQTT initialization failed; relay remains OFF");
}
