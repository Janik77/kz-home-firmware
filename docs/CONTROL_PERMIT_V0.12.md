# Firmware v0.12 — промежуточный control-permit adapter

Следующий этап: [service integration Core v0.22 + Firmware v0.12](CONTROL_SERVICE_INTEGRATION_V0.22_V0.12_RU.md)
прошла на настоящих временных PostgreSQL/Mosquitto TLS. Physical gate остаётся закрытым;
исторические ограничения ниже уточнены в новом отчёте.

Основа: чистый Firmware main `d32deaa` (v0.11.0), ветка
`feature/control-permit-firmware-v0.12`. Серверная сторона: Core `e73180a`,
[draft PR #29](https://github.com/Janik77/kz-home-core/pull/29).
Нормативная основа — Core
`docs/COMMAND_EXPIRY_CORE_FIRMWARE_CONTRACT_RU.md` и
`docs/CONTROL_AUTHORITY_CORE_V0.22_RU.md`; ранняя схема nonce через ACK/set не используется.

**Физическое управление закрыто с обеих сторон.** Worker создаёт budget с
`qualified=false`; никакое сообщение, версия firmware, status, local credentials
или сетевой флаг не открывает его. Это подготовка к совместной приёмке, не разрешение
flash/deploy или подключения нагрузки. Host-тесты положительного пути внедряют
лабораторные bounds и recording GPIO. Production bounds ещё не квалифицированы.

## 1. Topics и identity

Префикс `kzhome/v1/{house_id}/{device_id}` остаётся прежним.

| Суффикс | Направление | QoS | Retain |
|---|---|---|---|
| `/control/command` | Core → устройство | 1 | false |
| `/control/request` | устройство → Core | 1 | false |
| `/control/reply` | Core → устройство | 1 | false |
| `/ack` | устройство → Core | 1 | false |
| `/state` | устройство → Core | 1 | true |
| `/status` | устройство/broker LWT → Core | 1 | true |

Устройство подписывается только на собственные command/reply и ожидает оба SUBACK
QoS 1. Legacy `/set` не подписан, legacy engine отвергает физические команды.
Wildcard, чужой topic и retained control-пакеты не допускаются. Parser ограничен
4096 байтами, 512 tokens, глубиной 8; неизвестные и повторные ключи запрещены.

House/device binding обеспечивают точный topic, provisioned identity и broker ACL;
их нет в JSON как альтернативного источника identity. SHA-256 не является подписью.
Нужны verified TLS и directional ACL: только Core пишет command/reply, только
соответствующий физический principal пишет request. Компрометация Core/broker или
разрешение чужой записи reply разрушает предпосылку authority.

Будущий операторский ACL (в этой работе НЕ устанавливается):

```text
user PHYSICAL_DEVICE_USERNAME
topic read kzhome/v1/HOUSE_ID/DEVICE_ID/control/command
topic read kzhome/v1/HOUSE_ID/DEVICE_ID/control/reply
topic write kzhome/v1/HOUSE_ID/DEVICE_ID/control/request
topic write kzhome/v1/HOUSE_ID/DEVICE_ID/ack
topic write kzhome/v1/HOUSE_ID/DEVICE_ID/state
topic write kzhome/v1/HOUSE_ID/DEVICE_ID/status
```

Не выдавать physical principal доступ к legacy set; не использовать simulator/Core
credentials или одну identity одновременно на двух клиентах. ACL в Core — профильный
шаблон, не автоматическая регистрация principal и не доказательство live ACL.

## 2. Сообщения

Command — точные неизменные bytes, подготовленные и сохранённые Core:

```json
{"protocol_version":"v1","execution_profile":"deadline-permit-v1","core_epoch":"11111111-1111-4111-8111-111111111111","command_id":"22222222-2222-4222-8222-222222222222","correlation_id":"33333333-3333-4333-8333-333333333333","timestamp":"2026-11-02T10:00:00.123456+00:00","expires_at":"2026-11-02T10:00:30.123456+00:00","state":{"on":true}}
```

`expires_at` — исходный сохранённый E, а не время publish/retry. Firmware проверяет
канонический lowercase UUID, UTC (Z/+00:00), до 6 знаков дроби, строго boolean
`on` и `0 < E - timestamp <= 30 s`. Отсутствие/неверный E не имеет fallback.
До in-place JSON parsing вычисляется SHA-256 **точных полученных UTF-8 bytes**,
без завершающего NUL, whitespace normalization или повторной сериализации.

Request содержит ровно 9 полей:

```json
{"control_version":1,"type":"permit_request","execution_profile":"deadline-permit-v1","core_epoch":"11111111-1111-4111-8111-111111111111","command_id":"22222222-2222-4222-8222-222222222222","boot_id":"0102030405060708090a0b0c0d0e0f10","session_id":"1112131415161718191a1b1c1d1e1f20","nonce":"2122232425262728292a2b2c2d2e2f30","intent_sha256":"0be58faaa8eda6dbf704fa25f8efdfae33202bc491bd9bffca5bd58cac0f6614"}
```

Здесь ID иллюстративны; production получает 128-bit boot/session/nonce через
`esp_fill_random` после запуска Wi-Fi. Детерминированный RNG находится только в
host stub. Новые boot/session не восстанавливаются из flash. Session обновляется
при смене MQTT connection generation. Ожидается одна команда/один nonce за раз.

Grant повторяет binding-поля request, меняет `type` на `permit_grant` и добавляет
`core_time`, исходный `expires_at`, целочисленный `remaining_us`: ровно 12 полей.
Firmware требует `0 < remaining_us <= 2_000_000`, `remaining_us <= E-core_time`,
`timestamp <= core_time < E`, совпадение всех ID/digest/epoch и исходного E.
Core дополнительно ограничивает бюджет своим monotonic deadline и не перевыдаёт
разрешение другой boot/session/nonce. Denial — те же binding-поля, type
`permit_denied`, `reason` (unavailable/expired/invalid), всего 10 полей, без времени.
Все случаи отказа дают ноль GPIO; ACK lifecycle не используется как time-proof.

## 3. Monotonic budget и порядок исполнения

1. Проверить exact topic, session generation, валидность сетевых часов и NVS;
   hash/parse candidate. Исторический exact duplicate возвращает ACK из NVS и
   текущее state, без нового запроса/GPIO. Иной payload с тем же ID отвергается.
2. Запомнить `m0` до первой enqueue request. Не менять m0 при duplicate или MQTT
   retransmission. Ошибка enqueue отменяет ожидание; просрочка 2 s закрывает его.
3. Сверить grant с текущим nonce/boot/session/command/digest/epoch/E; потребить
   ожидание **до** NVS/GPIO. Второй reply больше не даёт исполнение.
4. Проверить квалификацию/допуск и budget, durable commit `pending`.
5. Снова прочитать monotonic clock после NVS. Проверить budget; при отказе
   durable `rejected/timeout`. Последняя проверка находится рядом с GPIO write
   в отдельном driver primitive; результат durable `applied`/`failed`, затем ACK/state.

Условная проверка с целочисленным округлением вверх:

```text
d = monotonic_now_us - m0
ceil(d * (1_000_000 + core_ppm) / (1_000_000 - device_ppm))
  + epsilon_us
  + ceil(gpio_us * (1_000_000 + core_ppm) / 1_000_000)
  < remaining_us
```

Равенство запрещает GPIO. d включает enqueue/outbox, оба направления MQTT,
планирование, parsing и NVS. Верхняя граница gpio_us должна покрывать **всё** после
последнего timer sample до завершения GPIO write, включая проверку допуска,
critical-section/IRQ/cache/driver latency. Сетевые поля не задают bounds.
Лабораторные 1000 ppm/2000 us/1000 us в тестах — примеры, не измерения платы.

Проверка E не опирается на совпадение абсолютного UTC ESP32 с Core. Наличие SNTP
нужно для TLS/status и обнаружения clock faults; оно не доказывает достоверный UTC.
Неизвестная квалификация запрещает GPIO даже при успешном SNTP/permit.

## 4. Reboot, reconnect, persistence

- Reconnect/disconnect/error, потеря IP/валидного времени, смена generation,
  ожидание ≥2 s или rollback таймера отменяют pending request. Старые сообщения
  не получают новый m0 в текущем ожидании, старый grant не совпадёт с новой session.
- Reboot создаёт новый boot ID; pending NVS → durable failed/hardware_failure,
  без повторного исполнения. Инициализация выхода OFF — отдельная boot policy.
- Core restart не отзывает уже доставленный permit мгновенно. Безопасность такого
  permit условно ограничена первоначальным R. Новый Core использует другую epoch,
  recovery fence и не принимает старую команду как новую. Сейчас обе стороны закрыты.
- NVS record v2 хранит epoch/digest/created/E и прежние ID/outcome. v0.11 record v1
  читается по прежнему layout и обновляется в v2 tombstone без стирания истории или
  house/device binding. Старый pending становится failed. Ошибка/corruption/unknown
  version запрещает новые команды, никакого auto-erase.
- 128 слотов, минимум 600 s по monotonic residency и local wall для terminal
  records. Core digest/expiry/одноразовый grant остаются обязательны после eviction.
  При full journal backpressure, без GPIO. Откат на v0.11 после записи v2 может
  заблокировать журнал старым reader; не стирать раздел ради downgrade.
- Commit failure до GPIO: ноль write. Сбой после GPIO: исход физически неизвестен,
  нет ACK успеха, storage закрывается; после reboot pending не replay.
- Поздний ACK/state не переводит Core timed_out обратно в applied. Deadline
  ограничивает command-driven GPIO write, не длительность ON/контакты реле.

## 5. Проверки и их пределы

Host runner: production parser/control/journal/engine/worker, настоящая SHA-256
(PSA в IDF, Windows CNG в проверенном host run), fake GPIO/NVS/clock/SDK.
На Linux host crypto использует OpenSSL; этот вариант в данной работе не запускался.

```powershell
python tests/run_host_tests.py --cc C:/path/to/zig.exe cc `
  --core-root C:/projects/kz-home-core `
  --core-site-packages C:/projects/kz-home-core/.venv/Lib/site-packages
```

| Проверка | Покрытие |
|---|---|
| Legacy parser/timestamp/reclaim/запрет set | 20 361 checks |
| Control parser/binding/budget/journal/faults | 40 205 checks; включая 40 000 malformed parser assertions |
| Реальная authority Core → C firmware через pipe | apply, expiry, wrong digest, delayed reply, delayed ACK; временная SQLite |
| Worker обеих плат | корректный grant всё равно закрыт production policy; legacy set, status, reconnect/clock fault |
| GPIO driver | 8 board/polarity/default/override variants |
| Compile guards | 31 unsafe configurations отвергнуты |
| CMake selector | 3 positive, 7 negative/environment cases |
| Core status compatibility | реальные schema/gateway, live/retained/malformed, обе платы |

Итоговый host runner прошёл 2026-11-03 с Zig 0.14.1 `cc`,
`-Wall -Wextra -Werror`; Core checkout `e73180a`, Python 3.12.7 с его существующими
зависимостями. Независимые ESP-IDF **6.0.3** builds прошли на тех же production
исходниках: classic ESP32 `0xe1e00` байт (39% app partition свободно), C6 `0x1080f0`
(28%). Сборки без local header/CA: GPIO boot OFF, сеть выключена. Vendor Kconfig
notices BT/FATFS не являются ошибками сборки. ELF/BIN/SDK configs/host outputs
остаются в ignored build directories и не входят в изменения.

Положительный host путь доказывает согласованность полей и алгоритма при заданных
fake bounds. Pipe не моделирует broker ACL/TLS/network scheduling, PostgreSQL locking,
реальную flash/NVS power-cut или физические часы. Успешная сборка не доказывает GPIO WCET.

## 6. Блокеры и порядок продолжения

1. Review Core draft/firmware diff; сохранить закрытые physical gates. Web v0.19 и
   программное demo используют отдельные software identities/legacy adapter без изменений.
   Старый Core v0.20/v0.21 физическим v0.12 управлять не сможет; legacy fallback нет.
2. Isolated broker + PostgreSQL: directional ACL, чужие/retained/reordered/repeated
   packets, Core crash/fence, client takeover, cancellation/deactivate, concurrency.
   Приватные credentials создаёт оператор вне репозитория, не этот код.
3. Квалифицировать Core clock/rate/UTC startup/suspend policy и single-authority
   deployment. В Firmware проверить timer continuity/sleep, RNG entropy, окончательный
   path до GPIO (IRAM/cache/IRQ/priority) и межъядерную синхронизацию generation/revocation.
   Нынешний portMUX сам по себе этих границ не доказывает; callback network clock внутри
   финальной проверки требует отдельного audit до включения.
4. На ESP32-WROOM и C6 снять timing traces под нагрузкой/flash stalls; power-cut NVS,
   reset/reconnect и E boundary. Проверить внешнюю электрическую boot OFF схему.
5. Только после совместной приёмки отдельным reviewed изменением задать доказанные
   bounds и точные operator-approved identities. Не добавлять runtime auto-enable.

Нет flash, deploy, обновления ACL/credentials, изменений Web/окружения наставника
или Firmware PR. Код готов для промежуточного review, physical contract не завершён.

## 7. Изменённые файлы

Финальная проверка перед Firmware PR (2026-11-03): полный diff от `main d32deaa`
проверен, runtime исправлений не потребовалось. Host runner повторно PASS с
20 361 legacy + 40 205 control checks, 5 Core pipe scenarios, обеими worker/status
suites, 8 GPIO variants, 31 compile guards и CMake isolation. Обе incremental
сборки IDF 6.0.3 PASS, размеры выше сохранены; объекты runtime актуальны.
Service integration в этом финальном проходе не перезапускалась, её результаты
относятся к отдельному отчёту. Упоминание отсутствия commit/PR в §6 описывает
предыдущий этап; теперь изменения подготовлены к одному коммиту и draft PR.
Добавлен [план аппаратной проверки](HARDWARE_VALIDATION_V0.12_RU.md), gate закрыт.

- Runtime: `main/control.c/.h` (новые), `protocol.c/.h`, `command_engine.c/.h`,
  `device_mqtt.c`, `journal.c/.h`, `relay.c/.h` в `kz_relay/`.
- Build/version: `kz_relay/CMakeLists.txt`, `kz_relay/main/CMakeLists.txt`,
  `kz_relay/main/config.h` (0.12.0). Board profiles, partitions и TLS policy сохранены.
- Новые tests: `test_control.c`, `control_bridge.c`, `control_crypto_host.c`,
  `test_control_integration.py`, stubs `esp_random.h`, `psa/crypto.h` в `tests/`.
- Обновлены tests: `run_host_tests.py`, `test_protocol.c`, `test_mqtt.c`,
  `test_core_compat.py`, `fake_platform.c/.h`. Устаревший pseudo-repository lifecycle
  test заменён реальной временной Core DB в control integration.
- Документация: этот файл и `README.md`; исторические v0.11/v0.10 отчёты сохранены.
