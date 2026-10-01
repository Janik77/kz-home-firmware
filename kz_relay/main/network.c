#include "network.h"
#include "config.h"
#include "protocol.h"
#include "journal.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <stdatomic.h>

static EventGroupHandle_t events;
static TimerHandle_t retry;
static atomic_uint retry_seconds = 1;
static const char *TAG = "kz_network";
#define HAS_IP BIT0
#define TIME_SYNC BIT1

bool network_config_valid(void)
{
    return strlen(KZ_WIFI_SSID) > 0 && strlen(KZ_WIFI_SSID) <= 32 &&
        strlen(KZ_WIFI_PASSWORD) >= 8 && strlen(KZ_WIFI_PASSWORD) <= 63 &&
        KZ_MQTT_HOST[0] && !strstr(KZ_MQTT_HOST, "://") && !strchr(KZ_MQTT_HOST, '/') &&
        !strchr(KZ_MQTT_HOST, '@') && KZ_MQTT_PORT > 0 && KZ_MQTT_PORT <= 65535 &&
        KZ_MQTT_USERNAME[0] && KZ_MQTT_PASSWORD[0] && KZ_TIME_SERVER[0] &&
        kz_topic_id(KZ_HOUSE_ID) && kz_topic_id(KZ_DEVICE_ID) &&
        strstr(kz_broker_ca_start, "-----BEGIN CERTIFICATE-----");
}

static void retry_wifi(TimerHandle_t timer)
{
    (void)timer;
    if (!network_has_ip()) {
        esp_wifi_connect();
        unsigned seconds = atomic_load(&retry_seconds);
        xTimerChangePeriod(retry, pdMS_TO_TICKS(seconds * 1000), 0);
        if (seconds < 32) atomic_compare_exchange_strong(&retry_seconds, &seconds, seconds * 2);
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xTimerStart(retry, 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, HAS_IP);
        xTimerChangePeriod(retry, pdMS_TO_TICKS(atomic_load(&retry_seconds) * 1000), 0);
        ESP_LOGW(TAG, "Wi-Fi disconnected; retry scheduled");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(events, HAS_IP); atomic_store(&retry_seconds, 1); xTimerStop(retry, 0);
        esp_netif_sntp_start();
        ESP_LOGI(TAG, "Wi-Fi has IP; synchronizing clock");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(events, HAS_IP); xTimerStart(retry, 0);
    } else if (base == NETIF_SNTP_EVENT && id == NETIF_SNTP_TIME_SYNC) {
        xEventGroupSetBits(events, TIME_SYNC);
    }
}

esp_err_t network_start(void)
{
    events = xEventGroupCreate();
    retry = xTimerCreate("wifi_retry", pdMS_TO_TICKS(1000), pdFALSE, NULL, retry_wifi);
    if (!events || !retry) return ESP_ERR_NO_MEM;
    esp_err_t err;
#define TRY(call) do { err = (call); if (err != ESP_OK) return err; } while (0)
    TRY(esp_netif_init()); TRY(esp_event_loop_create_default());
    if (!esp_netif_create_default_wifi_sta()) return ESP_FAIL;
    TRY(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    TRY(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    TRY(esp_event_handler_register(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, event_handler, NULL));
    esp_sntp_config_t time_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(KZ_TIME_SERVER);
    time_cfg.start = false;
    TRY(esp_netif_sntp_init(&time_cfg));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&cfg)); TRY(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi = {0};
    memcpy(wifi.sta.ssid, KZ_WIFI_SSID, strlen(KZ_WIFI_SSID));
    memcpy(wifi.sta.password, KZ_WIFI_PASSWORD, strlen(KZ_WIFI_PASSWORD));
    wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi.sta.pmf_cfg.capable = true;
    TRY(esp_wifi_set_mode(WIFI_MODE_STA)); TRY(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    TRY(esp_wifi_start());
#undef TRY
    return ESP_OK;
}

bool network_has_ip(void) { return events && (xEventGroupGetBits(events) & HAS_IP); }

int64_t network_now_ms(void)
{
    /* main calls this before worker startup; afterwards only the worker does.
     * Monotonic anchor prevents wall-clock rollback from reviving commands. */
    static int64_t base_wall, base_mono;
    static bool clock_fault;
    if (clock_fault || !events || !(xEventGroupGetBits(events) & TIME_SYNC)) return 0;
    struct timeval tv; gettimeofday(&tv, NULL);
    int64_t wall = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int64_t mono = esp_timer_get_time() / 1000;
    if (!base_wall) {
        if (wall < 1767225600000LL || wall < journal_clock_floor()) return 0;
        base_wall = wall; base_mono = mono;
    }
    int64_t now = base_wall + mono - base_mono;
    if (wall - now > 2000 || now - wall > 2000) {
        clock_fault = true; ESP_LOGE(TAG, "Clock discontinuity; commands inhibited until reboot"); return 0;
    }
    return now;
}

bool network_timestamp(char out[25])
{
    int64_t now = network_now_ms(); if (!now) return false;
    time_t sec = now / 1000; struct tm tm;
    gmtime_r(&sec, &tm);
    if (strftime(out, 20, "%Y-%m-%dT%H:%M:%S", &tm) != 19) return false;
    unsigned fraction = (unsigned)(now % 1000);
    out[19] = '.'; out[20] = '0' + fraction / 100;
    out[21] = '0' + (fraction / 10) % 10; out[22] = '0' + fraction % 10;
    out[23] = 'Z'; out[24] = 0; return true;
}
