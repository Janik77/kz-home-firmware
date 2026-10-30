#pragma once
#include "board.h"
#ifdef KZ_LOCAL_CONFIG
#include "device_config.h"
extern const char kz_broker_ca_start[] asm("_binary_kz_broker_ca_start");
#else
/* Safe unconfigured build, including CI: no network and no relay activation. */
#define KZ_WIFI_SSID ""
#define KZ_WIFI_PASSWORD ""
#define KZ_MQTT_HOST ""
#define KZ_MQTT_PORT 8883
#define KZ_MQTT_USERNAME ""
#define KZ_MQTT_PASSWORD ""
#define KZ_HOUSE_ID ""
#define KZ_DEVICE_ID ""
#define KZ_TIME_SERVER ""
static const char kz_broker_ca_start[] = "";
#endif
#define KZ_FIRMWARE_VERSION "0.10.0"
