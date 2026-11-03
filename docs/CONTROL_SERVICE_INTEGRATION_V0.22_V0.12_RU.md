# Проверка Core v0.22 + Firmware v0.12 на временных сервисах

2026-11-03. Core: `feature/command-expiry-core-v0.22`, `e73180a`, существующий
draft PR #29. Firmware: `feature/control-permit-firmware-v0.12`, база `d32deaa`,
прежние незакоммиченные изменения сохранены. Core/Web runtime не менялся.

**Результат: service integration пройдена, physical enablement запрещён.**
Положительный путь использует отдельный trusted test gate/clock и C recording
GPIO. Production physical gate Core и `qualified=false` Firmware сохранены.
Никаких flashing, deploy, новых PR, commit или merge в этом этапе.

## Настоящие сервисы и изоляция

- PostgreSQL **17.11** (`postgres:17-bookworm`), Mosquitto **2.1.2**
  (`eclipse-mosquitto:2`), Docker Engine 29.8.0. Использованы локально доступные images.
- Уникальные `kz-control-lab-<random>` контейнеры и сеть, label с ID конкретного
  запуска, случайные порты, опубликованные только на `127.0.0.1`.
- PostgreSQL data на новом tmpfs; никакого подключения existing/production/demo
  volumes или DB. Этот tmpfs не предназначен для Docker container restart durability.
- Собственные однодневные CA/server certificates с SAN `localhost`, отдельная
  неправильная CA для negative test. Уникальные случайные тестовые пароли; ни одного
  production credential. Все private inputs создаются в выделенном OS TEMP.
- Cleanup в `finally`: удаление только контейнеров/сети с совпадающим owner label,
  `docker rm -f -v`, проверка абсолютного TEMP target перед удалением сертификатов,
  паролей и каталога. После итогового запуска lab containers/networks/TEMP dirs отсутствуют.
- Docker Desktop был запущен для теста; daemon оставлен работающим, поскольку он
  обслуживает и другие контейнеры. Существующие KZ Home контейнеры не использовались,
  не останавливались, не перенастраивались и не удалялись. Окружение наставника не менялось.

## Результаты PostgreSQL

| Проверка | Результат |
|---|---|
| Создание схемы до `0006`, историческая command row, upgrade `0007` | Все поля прежнего ledger совпали до/после |
| Downgrade/upgrade при пустой control table | PASS, старый ledger сохранён |
| Новые control schema/ORM | Совпали; historical users drift описан ниже |
| CHECK profile/epoch/digest/payload | Неверные значения отвергнуты PostgreSQL |
| FK на ledger / RESTRICT удаления parent | Orphan row и удаление истории отвергнуты |
| Ошибка child insert в authority.prepare | Parent ledger тоже rollback, обе строки отсутствуют |
| Downgrade с control history | Отказ без удаления истории |
| Два concurrent authority.request | Ровно одна выдача permit |
| Два независимых SQL CAS с NULL predicate | Сумма rowcount ровно 1 |
| Повтор binding в другой command row | Отвергнут UNIQUE constraint |
| Новый Core subprocess | Пустые RAM windows/cache, новая epoch; old grant не перевыдаётся |
| Recovery при неверном boot UTC | До fence unavailable; после моделируемого fence timed_out |

Найдено существующее расхождение **до control migration**: `0003_auth_foundation`
создаёт и `uq_users_email`, и unique index `ix_users_email`, ORM описывает индекс.
PostgreSQL autogenerate предлагает убрать лишний constraint. Он оставлен нетронутым;
тест допускает только это точное историческое отличие и не скрывает control drift.
Изменять старую auth migration ради этого этапа не требуется; это отдельная cleanup-задача.

Полный Core pytest с явным `TEST_POSTGRESQL_URL` на эту временную DB:
**565 passed, 10 skipped, 59 warnings, 156.97 s**. Skipped проверки не считаются
пройденными. Сохранились прежние Starlette/AnyIO и Alembic deprecation warnings.

## Результаты настоящего MQTT/TLS/ACL

Использован Core `AiomqttClient` + `MQTTGateway`, а не FakeMQTTClient. Host-адаптер
получает command bytes через настоящий Mosquitto, передаёт их production C parser/
control/journal/engine, отправляет request и получает reply через broker. Lifecycle
ACK/state идут через тот же gateway в настоящий PostgreSQL. SDK/GPIO/NVS границы
Firmware подменены host doubles; это не ESP-MQTT/ESP32 hardware E2E.

| Проверка | Результат |
|---|---|
| Правильные CA/SAN и отдельный test principal | TLS connect и control round-trip PASS |
| Неправильные CA/SAN/password, anonymous | Соединение отвергнуто |
| Physical publish собственного server reply/command | Пакет не доставлен; server-only направления ACL соблюдены |
| Physical publish/read чужой command | Пакет не доставлен |
| До E, достаточный budget | Один recording GPIO write, ACK + state → applied |
| Production-like unqualified Firmware | Настоящий grant получен, GPIO write = 0 |
| Expired E | Core denial, GPIO = 0 (Core clock advance моделируется) |
| Delayed command | Короткий сохранённый E=600 ms, реальная задержка обработки 800 ms, denial/ноль GPIO |
| Delayed reply | Реальная задержка 2.1 s, host elapsed начислен monotonic budget, ноль GPIO |
| Duplicate request | Те же bytes grant, C/R/E не обновлены, один write |
| Replay reply/candidate/другой nonce | Нет второго write/нового permit |
| MQTT reconnect / Firmware reboot | Новые session/boot, replay старого reply через реальное соединение, ноль write |
| Firmware clock fault | Ожидание отменено, ноль write |
| Core authority + gateway reconstruction | Новый CONNECT/SUBSCRIBE, новая epoch, old request denial, fence |
| Bad digest | Core denial, ноль write |
| Late ACK/state | Доставлены через broker после E, receipt остался timed_out |
| Retained request bootstrap replay | Gateway после переподписки отверг пакет, durable binding не создан |
| Legacy physical set | Core guard + отдельный test ACL отказывают; host legacy operation inert |
| Software demo relay | Неизменённый simulator.Relay получил /set и вернул ACK/state через TLS; software state ON |

В fixture ACL для нового physical principal нет `/set`; deployed ACL не менялся.
Core server directions соответствуют tracked template. Software principal отдельный,
его identity не используется C physical adapter. Проверка software simulator не является
обновлением или запуском демоустановки наставника/Web.

MQTT v3 живому подписчику обычно передаёт RETAIN=false для первоначальной публикации,
даже если publisher сохранил пакет. Тест retained проверяет именно **bootstrap replay**,
где broker передаёт retained flag. Это не обещание распознавать флаг первоначального
publish на любой версии MQTT. Уникальные bindings/epoch/deadline обязательны независимо
от retain; корректный Firmware публикует request non-retained.

## Моделируемые границы и оставшиеся блокеры

- Core test clocks, UTC health, rate qualification, expiry advance, fence duration,
  wall jump, suspend со stopped timer, monotonic rollback и health loss — deterministic
  injections. ОС/host clock не менялись, реального suspend/resume не выполнялось.
- Новый Core subprocess и его PostgreSQL connection настоящие; ожидание recovery
  fence ускорено только fixture clock. Это не квалификация host suspend-aware clock.
- C host NVS остаётся RAM-backed persistent fake внутри процесса. Reboot моделирует
  повторную инициализацию при сохранении fake disk; real NVS/power-cut не проверены.
- Положительный C путь имеет искусственно заданные bounds и recording GPIO. Реальные
  rate/drift, IRAM/cache/IRQ/preemption, final-check-to-GPIO WCET и межъядерная
  синхронизация generation/revocation не доказаны. Требуются обе физические платы.
- Single-authority deployment/leader exclusion, startup UTC policy и suspend continuity
  остаются production-блокерами. Nonce не является attestation.
- Уже выданный permit нельзя мгновенно отозвать при Core restart: его ограничивает
  исходный R. Закрытые production gates и дальнейшая аппаратная приёмка обязательны.

Host-регрессия после изменения test RNG прошла: **20 361 legacy + 40 205 control
checks**, 5 прежних pipe integration scenarios, обе worker/status suites, 8 GPIO
variants, 31 negative compile guards, CMake selection/isolation. Ruff Core и двух
новых Python service fixtures прошёл. Runtime Firmware в этом этапе не менялся,
новые IDF builds не требовались; успешные 6.0.3 builds относятся к предыдущему этапу.

## Изменения и воспроизведение

Добавлены `tests/service_integration.py`, `tests/control_service_bridge.c`,
`tests/control_restart_probe.py`, этот отчёт. `tests/run_host_tests.py` получил
явные opt-in `--services`/`--full-core-services`. `tests/control_crypto_host.c`
использует системный RNG только при `KZ_SERVICE_RANDOM` для новых процессов service
fixture; прежние deterministic unit tests сохранены. Production RNG не менялся.

В процессе выявлен дефект fixture: одинаковая начальная deterministic RNG sequence
во вновь запускаемых C-процессах приводила к повтору boot/session/nonce. Core
правильно отклонял такие попытки. Добавлены системный RNG для service adapter и
проверка типа **каждого** ожидаемого grant/denial, чтобы отрицательные сценарии не
могли ошибочно пройти из-за постороннего отказа. Runtime bugs не обнаружены.

Из Firmware repo с Python окружением Core (уже установленными dependencies),
host C compiler и CMake; Docker должен быть доступен:

```powershell
& C:/projects/kz-home-core/.venv/Scripts/python.exe tests/run_host_tests.py `
  --cc C:/path/to/zig.exe cc `
  --core-root C:/projects/kz-home-core `
  --services --full-core-services
```

Обычный host runner без `--services` не запускает Docker. Все private test inputs
создаются только во время opt-in run и удаляются после него. Ключи, сертификаты,
пароли, логи и binaries в Git не добавлены. Firmware остаётся незакоммиченной;
Core commit/PR не изменялись. Готово к review результатов service integration,
**не к включению физического управления**.
