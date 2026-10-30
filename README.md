# KZ Home Relay v0.10.0

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
Значения freshness/dedup ниже — утверждённая policy физического relay v0.9.

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
задаются CMake-параметрами профиля; firmware — `0.10.0`.

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
idf.py -B build/esp32_wroom-v010 -D KZ_BOARD=esp32_wroom build
idf.py -B build/esp32c6-v010 -D KZ_BOARD=esp32c6 build
# Пример отдельного active-low build после проверки схемы и GPIO:
idf.py -B build/esp32_wroom-low -D KZ_BOARD=esp32_wroom -D KZ_RELAY_GPIO=26 -D KZ_RELAY_ACTIVE_HIGH=0 build
```

Linux/macOS, из `kz_relay`:

```sh
idf.py -B build/esp32_wroom-v010 -D KZ_BOARD=esp32_wroom build
idf.py -B build/esp32c6-v010 -D KZ_BOARD=esp32c6 build
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
idf.py -B build/esp32_wroom-v010 -D KZ_BOARD=esp32_wroom -p COM3 flash monitor
# Для C6 использовать ТОЛЬКО build/esp32c6-v010 и KZ_BOARD=esp32c6.
```

Linux/macOS: та же команда с реальным портом вместо COM3, например `/dev/ttyUSB0`.
Выход из monitor: Ctrl+]. В v0.10.0 hardware flash/acceptance не выполнялись.

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
- `network.c`: Wi-Fi station, reconnect, SNTP и часы для freshness.
- `device_mqtt.c`: TLS MQTT, SUBACK, сборка фрагментов, worker, ACK/state/status.
- `kz_relay.c`: безопасная инициализация и запуск модулей.

Последовательность: GPIO inactive / logical OFF → проверка local config → NVS и восстановление
журнала → Wi-Fi/IP → синхронизация UTC → MQTT TLS с настроенным offline LWT →
подписка на собственный `/set` → успешный SUBACK QoS 1 → retained текущее state →
retained online. Без времени TLS/MQTT и обработка команд не стартуют. Ожидание
yield-ит FreeRTOS, не препятствуя Wi-Fi reconnect.

Wi-Fi повторяет подключение с задержкой до 32 секунд без reboot. MQTT делает
автоматический reconnect через 5 секунд, clean session и повторную подписку
при каждом соединении. Неуспешный SUBACK/таймаут подписки не разрешает online;
worker перезапускает клиент с ограничением частоты. Callback не пишет GPIO/NVS:
он собирает сообщения в bounded очередь (4 сообщения, каждое до 16 KiB).
Сообщения из предыдущего соединения отбрасываются. MQTT outbox ограничен 32 KiB.

## Точный MQTT-контракт

Префикс всех топиков: `kzhome/v1/{house_id}/{device_id}`.

| Суффикс | Направление | QoS | Retain |
|---|---|---|---|
| `/set` | Core → ESP32 | подписка 1 | false |
| `/ack` | ESP32 → Core | 1 | false |
| `/state` | ESP32 → Core | 1 | true |
| `/status` | ESP32 / broker LWT → Core | 1 | true |

Telemetry не публикуется. Подписки с wildcard отсутствуют.

Команда ON (OFF отличается только `on:false`):

```json
{"command_id":"cmd_example","correlation_id":"corr_example","timestamp":"2026-09-30T12:00:00.000Z","state":{"on":true}}
```

После успешного исполнения публикуются ACK, затем state:

```json
{"command_id":"cmd_example","correlation_id":"corr_example","timestamp":"2026-09-30T12:00:00.010Z","status":"applied"}
{"timestamp":"2026-09-30T12:00:00.011Z","correlation_id":"corr_example","state":{"on":true}}
```

Оба ID в ACK и correlation ID в причинном state сохраняются без изменения
значений (JSON escaping может отличаться). `accepted` не нужен для синхронной
операции GPIO: сразу выдаётся терминальный результат. `applied` означает успех
GPIO API, **не измеренную обратную связь контактов/нагрузки**. Ошибка GPIO даёт
`failed` / `hardware_failure`. Boot/reconnect state сообщает фактическое текущее
логическое состояние и может не иметь correlation ID.

LWT устанавливается **до CONNECT**: `{"status":"offline"}`, QoS 1, retained.
Online содержит `status:"online"`, UTC `timestamp`, `last_seen`,
`heartbeat_interval_seconds:60` и optional metadata `protocol_version:"v1"`,
`firmware_version:"0.10.0"`, `hardware_model` выбранного профиля.
Online повторяется каждые 60 секунд; state не рассылается периодически без причины.
При потере питания/связи broker публикует LWT после обнаружения разрыва;
keepalive 30 секунд. Это не обещание мгновенного offline.

## Валидация, freshness и durable deduplication

До GPIO проверяются точный topic, полный размер/порядок фрагментов, корректный
UTF-8/JSON object, отсутствие duplicate JSON keys, ID, UTC timestamp, версия
(если передана), непустой state и строго boolean `on`.
Неизвестная capability → `rejected/unsupported_capability`; неправильный
тип/null → `rejected/invalid_value`; повреждённый envelope, пустой state,
неверная версия или retained set → `rejected/protocol_error`.
Unknown optional envelope metadata валидируется и игнорируется.

Ограничения: payload 16 KiB, вложенность 8, ID 128 UTF-8 байт, string 1024 байта,
не более 64 state fields. Дополнительные product limits: 512 JSON values/keys
на сообщение, embedded NUL запрещён, duplicate keys запрещены. Некорректный
JSON или отсутствующие/невалидные IDs отбрасываются без ACK: безопасно скопировать
оба ID невозможно. Невалидные команды никогда не меняют GPIO.

Утверждённая policy:

- Возраст новой команды **≤300 секунд**. Старше → `rejected/timeout`.
- Опережение времени **≤30 секунд**. Больше → `rejected/protocol_error`.
- Core может повторять intent в пределах 300 секунд с **тем же command_id**.
- Журнал сохраняет ID, correlation ID, исходный timestamp, desired on,
  время первой обработки и результат минимум **600 секунд** от первой обработки.

Журнал содержит 128 записей. Перед GPIO сначала коммитится `pending`; после GPIO
коммитится `applied`/`failed`; только после этого результат доступен для ACK.
Валидно идентифицируемые отклонённые команды также коммитятся перед ACK.
При сбое записи до GPIO действие не выполняется. При сбое записи после GPIO
выход может уже измениться: ACK об успехе не выдаётся, дальнейшее управление
блокируется до восстановления storage; текущий state публикуется по возможности.

После reset реле всегда OFF. Сохранённый `pending` превращается в durable
`failed/hardware_failure`: неизвестно, успел ли GPIO измениться до reset,
поэтому действие никогда не повторяется. Уже терминальные результаты сохраняются.
Повтор той же команды возвращает предыдущий ACK и **текущее** state, не вызывает
GPIO API. Например, исторический `applied` для ON после reset сопровождается
state OFF. Это не новый успешный запуск старой команды: терминальный ACK не
регрессирует, просроченная команда не исполняется повторно. Повторное использование
ID с другим correlation/timestamp/on отбрасывается без изменения результата.

Очистка ленивая, только при необходимости нового слота. Запись заменяется,
когда прошло ≥600 секунд по UTC **и** по monotonic времени нахождения записи в
RAM (после reset отсчитывается заново), а исходная команда уже просрочена.
Поэтому reboot продлевает защиту; запись отклонённой далёкой future-команды
может занимать слот дольше 600 секунд. При заполнении live-записями или ошибке
storage новые команды отбрасываются без GPIO и без недолговечного terminal ACK.
Это backpressure, не фиктивный успех; Core/оператору нужен retry с исходным ID
до expiry. Автоматического стирания и вытеснения живых записей нет.

Если MQTT outbox заполнен, ACK и причинный state остаются у worker для повторной
постановки с исходными ID; новые команды до этого не исполняются. Очередь может
отбрасывать сообщения при перегрузке. QoS 1 не заменяет command-level retry.
После полного reset пропавший ACK восстанавливается по повтору команды из NVS;
самостоятельной массовой отправки исторических ACK нет.

Часы: нужен доступный операторский SNTP. До синхронизации команды запрещены;
после синхронизации используется UTC, привязанный к monotonic clock. Начальный
UTC не может быть раньше последней durable обработки. Скачок wall clock более
2 секунд блокирует обработку до reboot; прошивка пытается отправить offline
перед остановкой MQTT. SNTP сам по себе не аутентифицирует источник времени:
используйте доверенную сеть/сервер. Прошивка не заменяет это отключением проверки
сроков сертификата, CA или hostname.

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

## Checklist первого hardware E2E

1. Проверить фактическую плату, flash ≥2 MiB, выбранный GPIO, полярность и безопасную
   нагрузку. Для classic GPIO26 предварителен. Измерить OFF при boot/reset;
   подготовить возможность отключения питания.
2. После отдельного Core follow-up зарегистрировать house/device с доверенной
   capability `on_off` (wire state `on:boolean`), привязанными к правильному дому.
   Использовать стабильный device ID; не выдавать classic ESP32 за C6.
3. Оператору развертывания предоставить физической плате приватный сетевой путь
   к Mosquitto TLS: текущий documented Compose держит брокер только во внутренней
   Docker-сети без опубликованного host-порта. Не открывать plaintext MQTT/anonymous.
   Для варианта с LAN port publication: подключить Mosquitto также к отдельной
   non-internal bridge-сети для ingress, сохранив существующую internal backend;
   опубликовать `LAN_BIND_IP:8883:8883` и разрешить firewall-доступ только из
   device LAN/VLAN. Не публиковать на `0.0.0.0` и не открывать PostgreSQL.
4. Обеспечить разрешение DNS-имени из Wi-Fi и соответствующий SAN сертификата.
   Текущая документация Core требует SAN `mosquitto`; LAN hostname/IP нельзя
   подставить с отключением hostname verification. При необходимости оператор
   отдельно меняет deployment/certificate; firmware этого не делает.
5. Создать уникального физического MQTT principal/password и directional ACL:
   read **только** `kzhome/v1/HOUSE/DEVICE/set`; write **только** собственные
   `/state`, `/ack`, `/status`. Текущий ACL содержит Core и simulator identities;
   аккаунт Core и общие пароли использовать нельзя. Не запускать симулятор и
   физическое реле одновременно с одной identity.
6. Заполнить локальный header, отдельно предоставить публичный CA, проверить
   доступность SNTP. Собрать конфигурируемый образ; проверить ignore перед flash.
7. Flash/monitor: подтвердить IP → UTC → verified TLS → SUBACK → state OFF → online.
   Неверный CA, hostname и пароль должны препятствовать работе, не включать GPIO.
8. Через Core выполнить ON/OFF; сопоставить command/correlation IDs, ACK, state,
   GPIO и запись в PostgreSQL. Проверить автоматизацию через существующий Core flow.
9. Проверить malformed/unsupported/null, старше 300s и future >30s: без изменения
   GPIO. Повторить command ID до/после reset: без повторного действия; boot OFF.
10. Проверить outage/recovery Wi-Fi и broker, resubscribe, 60s heartbeat, LWT,
    retained state после reconnect и сохранение ON при потере сети. Проверить
    power-cut до/после NVS commit/GPIO, заполнение журнала и восстановление после
    600s. Для этих тестов требуется реальное оборудование.

По DEVICE_PROTOCOL §19 текущий Core ещё не реализует command retries, heartbeat
expiry и freshness retained state. v0.20 уже содержит отдельный command lifecycle
с 30s deadline и требованием matching ACK + live correlated state. Firmware
не утверждает, что остальные серверные гарантии существуют, и не меняет Core.

## Отдельные follow-up задачи (вне v0.10.0)

- **Core onboarding:** добавить проверенный classic ESP32 profile. Сейчас Core
  разрешает только `esp32-c6-relay-v1`; metadata firmware не обходит этот gate.
- **LWT compatibility:** firmware отправляет `{"status":"offline"}`, а текущая
  Core `StatusEnvelope` требует `last_seen`. Offline может быть отвергнут;
  это известный незакрытый блокер network E2E. Wire behavior здесь не изменён.
- **Protocol/Core policy:** отдельно рассмотреть ID character policy, firmware
  300s freshness против Core 30s deadline, delivery/retries и heartbeat expiry.
- **Provisioning/security:** protected credential storage, rotation/revocation,
  secure boot/flash encryption и recovery после отдельного проектирования.
- **OTA:** signed updates и layout после проверки flash; текущий factory layout
  без OTA slots сохраняется вместе с журналом.
- **Radios/Web:** BLE Mesh, Matter, Thread и изменения Web — отдельные задачи.

## Проверка v0.10.0

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

Точный ACL для нового физического principal (заменить PLACEHOLDER-значения
согласованно с Core и local header; добавляет оператор, не эта прошивка):

```text
user PHYSICAL_DEVICE_USERNAME
topic read kzhome/v1/HOUSE_ID/DEVICE_ID/set
topic write kzhome/v1/HOUSE_ID/DEVICE_ID/state
topic write kzhome/v1/HOUSE_ID/DEVICE_ID/ack
topic write kzhome/v1/HOUSE_ID/DEVICE_ID/status
```

Пароль добавляется отдельно штатным операторским процессом Mosquitto.
После изменения port/network/ACL/password inputs оператор пересоздаёт брокер
по documented deployment workflow (входы копируются в runtime tmpfs на старте).
Ротация CA также требует обновить доверенный публичный CA на ESP32 и в Core.
Core wire-протокол менять не требуется.
