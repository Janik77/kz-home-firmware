# KZ Home Relay

Минимальная прошивка для проверки управления реле с помощью ESP32-C6 и
ESP-IDF. Реле подключается к `RELAY_GPIO`, определённому в
`kz_relay/main/kz_relay.c`, и переключается каждые две секунды.

## Сборка и прошивка

Запустите команды из каталога `kz_relay`:

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p COM_PORT flash monitor
```

Замените `COM_PORT` на порт подключённой платы (например, `COM3` в Windows или
`/dev/ttyUSB0` в Linux).
