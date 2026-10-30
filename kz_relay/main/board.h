#pragma once
#include "sdkconfig.h"

#if (defined(KZ_BOARD_esp32_wroom) + defined(KZ_BOARD_esp32c6)) != 1
#error "Select exactly one supported KZ board"
#endif

#if defined(KZ_BOARD_esp32_wroom)
#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "ESP32-WROOM board requires the esp32 target"
#endif
#define KZ_HARDWARE_MODEL "kz-relay-esp32-wroom"
#ifndef KZ_RELAY_GPIO
#define KZ_RELAY_GPIO 26 /* Provisional until board wiring is verified. */
#endif
/* Conservative allowlist: no straps, flash, UART, JTAG or input-only pins. */
#define KZ_SAFE_RELAY_GPIO(p) ((p) == 18 || (p) == 19 || (p) == 21 || \
    (p) == 22 || (p) == 23 || (p) == 25 || (p) == 26 || (p) == 27 || \
    (p) == 32 || (p) == 33)
#else
#if !defined(CONFIG_IDF_TARGET_ESP32C6)
#error "ESP32-C6 board requires the esp32c6 target"
#endif
#define KZ_HARDWARE_MODEL "kz-relay-esp32c6"
#ifndef KZ_RELAY_GPIO
#define KZ_RELAY_GPIO 2
#endif
/* Exclude straps, flash, USB and default UART/JTAG pins. Wiring still needs review. */
#define KZ_SAFE_RELAY_GPIO(p) ((p) == 0 || (p) == 1 || (p) == 2 || \
    (p) == 3 || (p) == 10 || (p) == 11 || (p) == 14 || \
    ((p) >= 18 && (p) <= 23))
#endif

#ifndef KZ_RELAY_ACTIVE_HIGH
#define KZ_RELAY_ACTIVE_HIGH 1
#endif
_Static_assert(KZ_RELAY_ACTIVE_HIGH == 0 || KZ_RELAY_ACTIVE_HIGH == 1,
               "Relay polarity must be 0 or 1");
_Static_assert(KZ_SAFE_RELAY_GPIO(KZ_RELAY_GPIO), "Unsafe/unsupported relay GPIO for this board");

#define KZ_RELAY_OFF_LEVEL (KZ_RELAY_ACTIVE_HIGH ? 0 : 1)
#define KZ_RELAY_ON_LEVEL (KZ_RELAY_ACTIVE_HIGH ? 1 : 0)
