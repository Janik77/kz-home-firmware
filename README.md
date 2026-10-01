# KZ Home Relay v0.9

Прошивка одного реле: ESP32-C6, ESP-IDF **6.0.x** (сборка проверена с 6.0.3),
ESP-MQTT **1.1.0**, GPIO **2**, активный HIGH. LOW означает OFF.
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
зафиксированы в `main/relay.c`; модель — `kz-relay-esp32c6`, firmware — `0.9.0`.

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
хеш закреплены в `dependencies.lock`. MQTT вынесен из IDF 6 в managed component
([Espressif](https://github.com/espressif/esp-idf/blob/master/docs/en/api-reference/protocols/mqtt.rst)).

PowerShell, из корня firmware:

```powershell
cd kz_relay
$kzSdkconfig = Join-Path (Get-Location) 'sdkconfig.local'
idf.py -B build -D "SDKCONFIG=$kzSdkconfig" set-target esp32c6
idf.py -B build -D "SDKCONFIG=$kzSdkconfig" build
idf.py -B build -D "SDKCONFIG=$kzSdkconfig" -p COM3 flash monitor
# Отдельное подключение монитора:
idf.py -B build -D "SDKCONFIG=$kzSdkconfig" -p COM3 monitor
```

Linux/macOS, из `kz_relay`:

```sh
idf.py -B build -D SDKCONFIG="$PWD/sdkconfig.local" set-target esp32c6
idf.py -B build -D SDKCONFIG="$PWD/sdkconfig.local" build
idf.py -B build -D SDKCONFIG="$PWD/sdkconfig.local" -p /dev/ttyUSB0 flash monitor
```

Замените порт своим. Выход из monitor: Ctrl+]. `set-target` нужен при первоначальной
подготовке/смене target; повторять его перед каждой сборкой не требуется.
После добавления/удаления локального header/CA выполните `idf.py reconfigure`,
затем `build` с тем же SDKCONFIG. CMake по умолчанию также выбирает `sdkconfig.local`.
Отслеживаемый `sdkconfig` оставлен как старый snapshot демо и для новой сборки
не используется; основа — `sdkconfig.defaults`.

`partitions.csv` рассчитан на flash не менее 2 MiB: приложение 0x170000 байт,
отдельный NVS-раздел `commands` 0x20000 байт с адреса 0x180000.
Не меняйте/не стирайте этот раздел при обновлениях. Не запускайте `erase-flash`
для обычного обновления: это уничтожит защиту от повторов. Первая установка
поверх другого layout требует операторской проверки разметки и содержимого
flash. При ошибке NVS прошивка ничего автоматически не форматирует.

## Архитектура и запуск

- `relay.c`: только GPIO 2 и текущее логическое состояние выхода.
- `protocol.c`: bounded JSON/UTF-8 parser, timestamp, freshness, сравнение
  команд, правила освобождения записей; не зависит от ESP-IDF.
- `command_engine.c`: проверки и порядок NVS → GPIO → NVS; сеть отделена.
- `journal.c`: persistent deduplication в стандартном ESP-IDF NVS.
- `network.c`: Wi-Fi station, reconnect, SNTP и часы для freshness.
- `device_mqtt.c`: TLS MQTT, SUBACK, сборка фрагментов, worker, ACK/state/status.
- `kz_relay.c`: безопасная инициализация и запуск модулей.

Последовательность: GPIO LOW/OFF → проверка local config → NVS и восстановление
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
`firmware_version:"0.9.0"`, `hardware_model:"kz-relay-esp32c6"`.
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
нужна корректная внешняя схема/pull-down; boot-ROM и электрические переходные
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

Результат проверки текущего v0.9 (2026-10-01): **20 242 assertions прошли** с
Zig 0.14.1 `cc` (`-Wall -Wextra -Werror`), также прошёл запуск с
`-fsanitize=undefined -fno-sanitize-recover=all`. GCC 15.2.0 `-fanalyzer`
проверил все 7 production C-модулей без диагностик. ESP-IDF 6.0.3 успешно
собрал ESP32-C6 ELF/BIN и bootloader: приложение **0x106a40 байт**, 29% раздела
свободно. Это unconfigured build без local header/CA, с выключенной сетью.
В SDK были некритичные Kconfig notices BT/FATFS; BT и OpenThread выключены.
`git diff --check` прошёл. Hardware, реальная flash/NVS при power-cut,
Wi-Fi, TLS handshake и live MQTT/Core E2E **не запускались**.

## Checklist первого hardware E2E

1. Проверить плату ESP32-C6, flash ≥2 MiB, GPIO 2, активный HIGH и безопасную
   нагрузку. Измерить OFF при boot/reset; подготовить возможность отключения питания.
2. В Core зарегистрировать house/device и доверенную relay capability `on:boolean`
   (writable), привязанные к правильному дому. Использовать стабильный device ID.
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
expiry, полноценное отслеживание pending ACK и freshness retained state.
Firmware не утверждает, что эти серверные гарантии уже существуют, и не меняет Core.

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
