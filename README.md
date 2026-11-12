# KZ Home Relay v0.12.0 — промежуточная реализация

**Physical control закрыт:** production не исполняет ни legacy `/set`, ни новый
permit до квалификации временных границ и совместной приёмки. Это не готовый
образ для физического commissioning. Текущий контракт и блокеры:
[CONTROL_PERMIT_V0.12.md](docs/CONTROL_PERMIT_V0.12.md). Software demo наставника
и Web не меняются; установленный Core v0.20 обновлять сейчас не требуется.

План будущих испытаний обеих плат, оборудование и границы готовности образов:
[HARDWARE_VALIDATION_V0.12_RU.md](docs/HARDWARE_VALIDATION_V0.12_RU.md).
Финальная software-проверка v0.12: 20 361 legacy + 40 205 control checks,
5 Core pipe scenarios, обе worker/status suites, 8 GPIO variants, 31 compile guards
и CMake isolation — PASS. Повторные incremental builds ESP-IDF 6.0.3 — PASS:
ESP32 `0xe1e00`, C6 `0x1080f0`; runtime sources не потребовали пересборки объектов.
Оба образа без local config/CA, сеть выключена. Для положительных физических
timing/NVS испытаний отдельный диагностический harness ещё не подготовлен.

Прошивка одного реле: classic ESP32-WROOM-32 (основная плата) и ESP32-C6,
ESP-IDF **6.0.x**, ESP-MQTT **1.1.0**. Один общий исходный код, отдельные бинарники.
USB-C/CH340C не означает ESP32-C6: для имеющейся платы нужен target `esp32`.

| KZ_BOARD | IDF target | Hardware metadata | GPIO по умолчанию |
|---|---|---|---|
| `esp32_wroom` (по умолчанию) | `esp32` | `kz-relay-esp32-wroom` | **26, предварительно** |
| `esp32c6` | `esp32c6` | `kz-relay-esp32c6` | 2, сохранён из v0.9 |

Оба профиля по умолчанию active-high. `KZ_RELAY_ACTIVE_HIGH=0` выбирает active-low.
GPIO26 — только кандидат: до подключения проверить разводку конкретной платы.
Размер flash, ревизия платы и электрическое поведение **не проверены**.
Автоматического переключения нет. Matter/Zigbee/Thread/BLE, OTA и provisioning
не реализованы. Нормативный wire-контракт: соседний
[`kz-home-core/DEVICE_PROTOCOL.md`](../kz-home-core/DEVICE_PROTOCOL.md), v1.
Историческая policy v0.11 описана в docs/MQTT_COMPAT_V0.11.0.md; legacy freshness не разрешает GPIO в v0.12.

## Локальная конфигурация

1. Создайте `kz_relay/local/` и скопируйте туда
   `kz_relay/config/device_config.h.example` как `device_config.h`.
2. Заполните значения локально. Пароли — C string literals с корректным escaping.
3. Оператор должен отдельно предоставить **публичный CA** брокера в
   `kz_relay/local/broker_ca.pem` (PEM). Прошивке не нужны server/CA private key,
   MQTT password database, JWT или клиентский сертификат.

| Макрос | Назначение |
|---|---|
| `KZ_WIFI_SSID` | SSID, 1–32 байта |
| `KZ_WIFI_PASSWORD` | Пароль WPA2/WPA3-Personal, 8–63 байта; открытая сеть не поддерживается |
| `KZ_MQTT_HOST` | Доступное ESP32 DNS-имя брокера, соответствующее SAN сертификата; без схемы, пути, credentials |
| `KZ_MQTT_PORT` | TLS-порт, по умолчанию 8883 |
| `KZ_MQTT_USERNAME`, `KZ_MQTT_PASSWORD` | Индивидуальная учётная запись физического устройства |
| `KZ_HOUSE_ID`, `KZ_DEVICE_ID` | Зарегистрированные в Core канонические ID; 1–128 ASCII символов `[A-Za-z0-9_-]` |
| `KZ_TIME_SERVER` | Доступный операторский SNTP-сервер для UTC |

`device_id` стабилен, client ID равен `kz-{device_id}`. Пароли можно менять
пересборкой/прошивкой без смены identity и стирания журнала. GPIO и полярность
задаются CMake-параметрами профиля; firmware — `0.12.0`.

`local/`, локальные sdkconfig, build, managed components, `.env` и типичные
сертификаты/ключи игнорируются Git. Шаблон содержит только пустые значения.
Нет реальных адресов, паролей и сертификатов в исходниках. Wi-Fi driver хранит
конфигурацию в RAM. Локальные secrets и CA включаются в бинарник при сборке:
защищайте локальный header, build-каталог, ELF/BIN и логи как чувствительные
артефакты. Flash encryption / secure boot пока не настроены; физическое чтение
flash может раскрыть credentials. Будущее provisioning должно заменить источник
конфигурации, сохранив identity и журнал дедупликации.

Если header или CA отсутствует, проект собирается в безопасный непровижененный
образ: GPIO OFF, сеть выключена, фиксированное сообщение в логе. Ошибочная
конфигурация также блокирует запуск сети. Это **не** образ для hardware E2E.
Не включайте диагностический DEBUG/VERBOSE сетевых библиотек с реальными secrets.

## Сборка, flash, monitor

Автоматические сборки обеих плат на закреплённом ESP-IDF 6.0.3 и команды
повторения: [ESP_IDF_CI_RU.md](docs/ESP_IDF_CI_RU.md). Это проверка сборки
без local config, не аппаратная квалификация.

Активируйте ESP-IDF 6.0.x в терминале через его штатный export/activation script.
Первой сборке нужен доступ к Espressif Component Registry для MQTT; версия и
хеш закреплены в `dependencies.lock.esp32` / `dependencies.lock.esp32c6`.
Старый `dependencies.lock` оставлен как snapshot v0.9 и не используется.
MQTT вынесен из IDF 6 в managed component
([Espressif](https://github.com/espressif/esp-idf/blob/master/docs/en/api-reference/protocols/mqtt.rst)).

PowerShell, из корня firmware (только сборка; hardware пока не прошивать):

```powershell
# На этой машине установленный activation script:
. C:/Espressif/tools/Microsoft.v6.0.3.PowerShell_profile.ps1
cd kz_relay
idf.py -B build/esp32_wroom-v012 -D KZ_BOARD=esp32_wroom build
idf.py -B build/esp32c6-v012 -D KZ_BOARD=esp32c6 build
# Пример отдельного active-low build после проверки схемы и GPIO:
idf.py -B build/esp32_wroom-low -D KZ_BOARD=esp32_wroom -D KZ_RELAY_GPIO=26 -D KZ_RELAY_ACTIVE_HIGH=0 build
```

Linux/macOS, из `kz_relay`:

```sh
idf.py -B build/esp32_wroom-v012 -D KZ_BOARD=esp32_wroom build
idf.py -B build/esp32c6-v012 -D KZ_BOARD=esp32c6 build
```

`set-target`, `fullclean` и `erase-flash` для выбора профиля не нужны.
Не используйте один build-каталог для разных плат: CMake отклоняет смену платы
или несовпадающий target (в том числе `IDF_TARGET` из environment).
Каждый build хранит свой `sdkconfig` внутри build-каталога. Старые корневые
`sdkconfig`/`sdkconfig.local` не используются. Defaults: общий файл плюс
`boards/sdkconfig.defaults.<target>`. Managed MQTT общий, версия одинакова;
lock-файлы разделены по target. Сборки разных target запускайте последовательно.
После добавления/удаления local header/CA выполните `reconfigure`, затем `build`
с теми же `-B` и `-D KZ_BOARD`.

Будущая процедура flash **после отдельного разрешения и проверки оборудования**:

```powershell
# COM3 — пример; проверить выбранный порт и target, отключить реле/нагрузку.
idf.py -B build/esp32_wroom-v012 -D KZ_BOARD=esp32_wroom -p COM3 flash monitor
# Для C6 использовать ТОЛЬКО build/esp32c6-v012 и KZ_BOARD=esp32c6.
```

Linux/macOS: та же команда с реальным портом вместо COM3, например `/dev/ttyUSB0`.
Выход из monitor: Ctrl+]. В v0.12 hardware flash/acceptance не выполнялись.
Старые build-каталоги/artifacts v0.10.0 и private configuration сохраняются.

Classic build использует DIO/40 MHz и 2 MiB как консервативные **build assumptions**,
не как измеренные параметры платы. До flash проверить размер/тип flash и layout.
Увеличение flash не добавляет OTA автоматически.

`partitions.csv` рассчитан на flash не менее 2 MiB: приложение 0x170000 байт,
отдельный NVS-раздел `commands` 0x20000 байт с адреса 0x180000.
Не меняйте/не стирайте этот раздел при обновлениях. Не запускайте `erase-flash`
для обычного обновления: это уничтожит защиту от повторов. Первая установка
поверх другого layout требует операторской проверки разметки и содержимого
flash. При ошибке NVS прошивка ничего автоматически не форматирует.

## Архитектура и запуск

### Электрическая безопасность и board configuration

`KZ_RELAY_GPIO` и `KZ_RELAY_ACTIVE_HIGH` — build-time параметры, не credentials.
`main/board.h` отклоняет несовпадающие board/target и пины вне консервативного
allowlist. Для classic ESP32 разрешены 18, 19, 21–23, 25–27, 32–33;
GPIO2 не используется и не разрешён для реле. Strapping 0/2/5/12/15, flash 6–11,
UART 1/3, непроверенные memory pins 16/17 и input-only 34–39 исключены.
Для C6 сохранён GPIO2; чужую разводку на classic ESP32 переносить нельзя.

Приложение сначала записывает inactive level и только затем включает output:
active-high OFF=LOW, active-low OFF=HIGH. До успешной инициализации команды GPIO
запрещены; ошибка записи не меняет cached logical state. После reset ON не
восстанавливается из журнала. При потере сети последнее состояние сохраняется.

**Software OFF не гарантирует электрический OFF во время boot-ROM/reset,
brownout или до запуска приложения.** Нужна проверенная внешняя схема:
pull-down для active-high / pull-up для active-low, с учётом реального входа и
порядка питания relay module. Не подавать 5 V на GPIO и не питать катушку от GPIO.
Тип, pinout, токи, logic thresholds и наличие драйвера у реле пока неизвестны.

Первый физический тест: USB data cable, LED с последовательным резистором
примерно 1 kΩ или logic probe, мультиметр; oscilloscope/logic analyzer для
коротких импульсов boot/reset. Реле подключать только после проверки интерфейса,
с подходящим питанием/драйвером и низковольтной тестовой нагрузкой. Сеть 230 V
для первоначальных тестов не использовать. Сборка не является hardware acceptance.

- `boards/select.cmake`, `main/board.h`: target, metadata, GPIO и полярность.
- `relay.c`: выбранный GPIO и текущее логическое состояние выхода.
- `protocol.c`: bounded JSON/UTF-8 parser, timestamp, freshness, сравнение
  команд, правила освобождения записей; не зависит от ESP-IDF.
- `command_engine.c`: проверки и порядок NVS → GPIO → NVS; сеть отделена.
- `journal.c`: persistent deduplication в стандартном ESP-IDF NVS.
- `control.c`: raw SHA-256, boot/session/nonce и одноразовое ожидание permit.
- `network.c`: Wi-Fi station, reconnect, SNTP и часы для TLS/status.
- `device_mqtt.c`: TLS MQTT, SUBACK, сборка фрагментов, worker, ACK/state/status.
- `kz_relay.c`: безопасная инициализация и запуск модулей.

Последовательность: GPIO inactive / logical OFF → проверка local config → NVS и восстановление
журнала → Wi-Fi/IP → синхронизация UTC → MQTT TLS с настроенным offline LWT →
подписки на собственные `/control/command` и `/control/reply` → оба SUBACK QoS 1 → retained текущее state →
retained online. Без времени TLS/MQTT и обработка команд не стартуют. Ожидание
yield-ит FreeRTOS, не препятствуя Wi-Fi reconnect.

Wi-Fi повторяет подключение с задержкой до 32 секунд без reboot. MQTT делает
автоматический reconnect через 5 секунд, clean session и повторную подписку
при каждом соединении. Неуспешный SUBACK/таймаут подписки не разрешает online;
worker перезапускает клиент с ограничением частоты. Callback не пишет GPIO/NVS:
он собирает сообщения в bounded очередь (4 сообщения, каждое до 4096 байт).
Сообщения из предыдущего соединения отбрасываются. MQTT outbox ограничен 32 KiB.

## Точный MQTT-контракт v0.12

Префикс `kzhome/v1/{house_id}/{device_id}` сохранён. Устройство читает только
`/control/command`, `/control/reply`, пишет `/control/request`; всё QoS 1, non-retained.
Существующие `/ack`, `/state`, `/status` сохраняют прежние схемы/QoS/retain.
Legacy `/set` не подписан и не разрешает GPIO. Старый Core не управляет v0.12.

Точная схема command/request/grant/denied, raw-byte SHA-256, исходный сохранённый
Core deadline, одноразовый nonce, boot/session binding и алгоритм monotonic budget:
[CONTROL_PERMIT_V0.12.md](docs/CONTROL_PERMIT_V0.12.md). Не использовать ACK/set
для time-proof. Unknown/duplicate keys, отсутствующий E, неправильные ID/digest,
retained/чужой topic и grant старой session отвергаются без GPIO.

Production budget всегда `qualified=false`: успешные TLS, SNTP, status или permit
не открывают physical gate. Проверки положительного пути существуют только с
лабораторными bounds. Последовательность будущего квалифицированного пути:
проверка budget → durable pending → повторная проверка после NVS → финальная
проверка около GPIO → durable outcome → прежние ACK/state. До доказательства
GPIO WCET/rate/suspend/revocation bounds этот путь не допускается в production.

NVS journal v2 сохраняет epoch/digest/E вместе с прежними ID/outcome. Старые v1
записи обновляются без erase; pending после reset становится failed и никогда
не исполняется снова. Exact duplicate возвращает исторический ACK и текущее
state; иной payload с тем же ID отвергается. Full/corrupt NVS и ошибки commit
закрывают управление. Downgrade reader v0.11 может не принять v2 journal: не
стирать историю для обхода этой защиты. Разметка flash остаётся прежней.

Reconnect/reboot/clock fault инвалидируют ожидание permit. Deadline ограничивает
первую запись GPIO по команде, не время нахождения нагрузки во включённом состоянии.
Исторический `applied` не означает измеренную обратную связь контактов. Поздний
ACK не отменяет Core `timed_out`; возможен unknown execution.

LWT/offline остаётся историческим наблюдением `last_seen`, без выдуманного времени
disconnect. Online содержит `timestamp`, `status`, `last_seen`, heartbeat 60 s,
protocol v1 и firmware `0.12.0`/board metadata. Проверка initial UTC/SNTP нужна
для TLS/status и не доказывает абсолютные часы ESP32 или право на GPIO.
История status/LWT: [аудит v0.11.0](docs/MQTT_COMPAT_V0.11.0.md).

## Поведение при отказах

При потере Wi-Fi/MQTT реле **сохраняет последний уровень**, включая ON.
Таймер отключения нагрузки не реализован. При reset уровень устанавливается OFF,
восстановления ON из flash нет. Для безопасного уровня **до исполнения app_main**
нужна корректная внешняя схема/bias по полярности; boot-ROM и электрические переходные
процессы требуют измерения на реальной плате.

TLS использует только предоставленный CA и проверяет цепочку, срок действия и
имя `KZ_MQTT_HOST`. Нет insecure fallback, hostname override или mTLS.
Небезопасные TLS Kconfig options запрещены compile-time проверкой.
Логи приложения не содержат credentials или полных MQTT payloads.

## Проверки без оборудования

Из корня репозитория, с обычным host C-компилятором:

```sh
python tests/run_host_tests.py --cc gcc
# Или clang / переносимый Zig:
python tests/run_host_tests.py --cc clang
python tests/run_host_tests.py --cc /path/to/zig cc
git diff --check
```

Тесты компилируют **production** `protocol.c`, `command_engine.c`, `journal.c`.
Только GPIO, monotonic clock и NVS API заменены fake-реализациями. Проверяются
ON/OFF schema, Unicode/escaping, malformed/duplicate keys, nesting/size,
границы freshness, durable duplicates/reset, смена payload с тем же ID,
переполнение/очистка, ошибки commit до и после GPIO, crash с pending и отказ GPIO.
Дополнительно выполняются 20 000 детерминированных malformed-input проб.
Это не проверка физической flash/NVS при отключении питания.

Историческая проверка v0.9 (2026-10-01): **20 242 assertions прошли** с
Zig 0.14.1 `cc` (`-Wall -Wextra -Werror`), также прошёл запуск с
`-fsanitize=undefined -fno-sanitize-recover=all`. GCC 15.2.0 `-fanalyzer`
проверил все 7 production C-модулей без диагностик. ESP-IDF 6.0.3 успешно
собрал ESP32-C6 ELF/BIN и bootloader: приложение **0x106a40 байт**, 29% раздела
свободно. Это unconfigured build без local header/CA, с выключенной сетью.
В SDK были некритичные Kconfig notices BT/FATFS; BT и OpenThread выключены.
`git diff --check` прошёл. Hardware, реальная flash/NVS при power-cut,
Wi-Fi, TLS handshake и live MQTT/Core E2E **не запускались**.

## Следующая совместная приёмка (пока заблокирована)

Сначала review, isolated MQTT/PostgreSQL и квалификация временных границ, затем
аппаратные тесты на обеих платах. Полный порядок и критерии:
[CONTROL_PERMIT_V0.12.md §6](docs/CONTROL_PERMIT_V0.12.md#6-блокеры-и-порядок-продолжения).
До этого не выдавать production permit, не выполнять flash/deploy и не подключать нагрузку.

Нужны approved Core inventory/claim/house binding/activation (`esp32-relay-v1` или
`esp32-c6-relay-v1`), отдельная физическая identity, verified TLS/SAN/private LAN
path, индивидуальный principal с directional control ACL и доступный локальный
SNTP. Firmware не создаёт credentials/ACL и не открывает host-порт брокера. Не
использовать simulator identity. Не менять внутреннюю Docker-сеть наставника.
Проверки CA/hostname/пароля, GPIO/полярности/flash layout и внешнего boot OFF bias
остаются обязательными. Текущий source не разрешает физическое управление даже
после ручного заполнения private header.

## Отдельные follow-up задачи

- **Закрыто отдельно:** Core v0.21 (`6464bd8`) добавил `esp32-relay-v1` наряду с
  `esp32-c6-relay-v1`. Нужны approved identity, house binding и activation;
  metadata firmware не обходит admission gate.
- **Закрыто в v0.11.0:** LWT/offline содержит обязательный `last_seen` и проходит
  существующую Core schema. Историческое время, stale retained availability и
  отсутствие heartbeat expiry не превращаются в receipt-time guarantees.
- **Protocol/Core policy:** control adapter v0.12 подготовлен; квалификация bounds и
  physical acceptance заблокированы. Heartbeat expiry остаётся отдельной задачей.
- **Provisioning/security:** protected credential storage, rotation/revocation,
  secure boot/flash encryption и recovery после отдельного проектирования.
- **OTA:** signed updates и layout после проверки flash; текущий factory layout
  без OTA slots сохраняется вместе с журналом.
- **Radios/Web:** BLE Mesh, Matter, Thread и изменения Web — отдельные задачи.

## История: проверка v0.11.0

20 245 protocol/command/journal checks, board/polarity matrix и MQTT worker tests
для обеих плат. Захваченные payloads проходят **реальные** Core schema/gateway,
включая live/retained status и malformed input. Core читается без DB/network и
без записи bytecode. Независимые IDF 6.0.3 builds PASS: ESP32 `0xe0b10` (39% free),
C6 `0x106b70` (29% free), без local config/CA и без hardware acceptance.
Команда запуска, deadline counterexample и ограничения:
[MQTT_COMPAT_V0.11.0.md](docs/MQTT_COMPAT_V0.11.0.md).

## История: проверка v0.10.0

Проверено 2026-10-30 на ESP-IDF **6.0.3**, GCC **15.2.0**, CMake **4.0.3**,
Ninja **1.12.1**, host compiler Zig **0.14.1 cc**:

| Проверка | Результат |
|---|---|
| Новый `build/esp32_wroom-v010`, target esp32 | ELF/BIN + bootloader + partition table; app `0xe0a10`, 39% свободно |
| Новый `build/esp32c6-v010`, target esp32c6 | ELF/BIN + bootloader + partition table; app `0x106a60`, 29% свободно |
| Protocol/freshness/journal/engine | 20 242 checks PASS |
| GPIO/polarity/fault matrix | 8 executable variants PASS |
| Unsafe compile configurations | 31 отклонены ожидаемыми guards |
| CMake selection/isolation | 3 valid и 7 invalid/environment cases PASS |

Обе IDF сборки без local header/CA: сеть выключена. Metadata и compiler flags
соответствуют выбранным board/target/GPIO. IDF вывел некритичные Kconfig notices
BT/FATFS. Hardware, flash, live Wi-Fi/TLS/MQTT/Core/Web E2E не проверялись.

Host runner `tests/run_host_tests.py` использует настоящий parser/journal/engine
и настоящий `relay.c` с recording GPIO stub. Он проверяет оба профиля, обе
полярности, default/override GPIO, OFF-before-output, ошибки GPIO и запрет apply
после ошибки init. Негативные compile/CMake проверки отклоняют опасные GPIO,
неизвестные профили, target mismatch, shared build и невалидные параметры.
Передавайте `--cmake <path>` и `--cc <compiler> [cc]`, если инструменты не в PATH.
Эти тесты не моделируют физическую relay module или reset electrical waveform.

Актуальный control ACL и ограничения commissioning: [CONTROL_PERMIT_V0.12.md](docs/CONTROL_PERMIT_V0.12.md).
