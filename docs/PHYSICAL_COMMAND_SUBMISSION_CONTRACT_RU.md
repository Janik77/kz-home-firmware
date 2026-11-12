# Проект физического command submission: закрытый admission, durable intent и permit

Статус: **проект для review, не реализованный physical HTTP путь и не разрешение GPIO**.
Этот PR меняет только документацию. Production gates, протокол, build definitions,
оборудование и deploy не меняются. Положительный actual-worker тест не добавлен:
его isolation proof пока отсутствует; требования к нему приведены ниже.

## База, зависимости и доказательства

- Firmware PR #9, `codex/http-worker-e2e`,
  `a0c5bb948f3c267140673a5edd4ee67c9b259cd3`, поверх PR #7
  `75bb97dc088426aee9984ca8b14680ebb62903f8`. Этот PR направлен в ветку #9;
  цепочка review: #7 → #9 → этот проект. После merge родителей нужен retarget.
- Core PR #32, `codex/core-ci`,
  `159eb98f2349b1aa2b8afd313c3a3f1fbf9b6bd2`. В нём изменены CI и migration tests;
  runtime соответствует main `2359807c2781ce956ed43bc603772b66dd27955b`.
- [Проверенный CI PR #9](https://github.com/Janik77/kz-home-firmware/actions/runs/43255540422):
  реальные PostgreSQL/Mosquitto TLS, TCP HTTP software ON/OFF, настоящий C worker
  rejected ACK и ноль GPIO, полный Core pytest 700 passed / 10 skipped.
  Это не доказательство положительного physical HTTP/worker исполнения.
- [Описание разрыва](HTTP_WORKER_GAP_RU.md),
  [существующий permit contract](CONTROL_PERMIT_V0.12.md),
  [service E2E](SERVICE_E2E_CI_RU.md) остаются исходными материалами.

## 1. Текущее состояние и предлагаемая граница изменений

| Граница | Уже в исходниках | Требуется для будущего physical submission |
|---|---|---|
| HTTP | `app/api/commands.py` → `CommandService.submit`; physical contract guard возвращает 409 | Отдельный `submit_control` внутри service, с прежним закрытым admission; никакого fallback в legacy `/set` |
| Ledger | Unique house/user/idempotency hash, один pending на device | Сохранить эти constraints и транзакционно соединить authorization/intent с prepare |
| Authority | `prepare` атомарно сохраняет ledger + exact candidate/hash; `stage` помечает dispatch до I/O | HTTP должен вызвать именно prepare/stage, а не сначала сохранить legacy row и затем «усыновить» её |
| MQTT | `send_control_command`, request/grant, exact topic и directional ACL | Сохранить QoS 1/non-retained control; не менять wire v1 ради HTTP маршрутизации |
| Worker | `device_mqtt.c:command` задаёт `qualified=false` | Остаётся false; будущая qualification требует отдельного review и доказательств |
| GPIO | Intent commit → повторный budget check → GPIO → terminal journal | Сохранить неизвестный исход при сбое после GPIO, не выдавать ложный applied |

Исходники Core доступны на закреплённом commit:
[submission](https://github.com/Janik77/kz-home-core/blob/159eb98f2349b1aa2b8afd313c3a3f1fbf9b6bd2/app/services/command_service.py),
[authority](https://github.com/Janik77/kz-home-core/blob/159eb98f2349b1aa2b8afd313c3a3f1fbf9b6bd2/app/services/control_authority.py),
[clock](https://github.com/Janik77/kz-home-core/blob/159eb98f2349b1aa2b8afd313c3a3f1fbf9b6bd2/app/services/control_clock.py),
[physical guard](https://github.com/Janik77/kz-home-core/blob/159eb98f2349b1aa2b8afd313c3a3f1fbf9b6bd2/app/services/onboarding_service.py).

## 2. Нормативные инварианты будущей реализации

1. **Identity и admission.** HTTP authentication/RBAC, house/device binding, active
   inventory и trusted execution profile проверяются сервером. Metadata, client JSON,
   software allowlist, SNTP или успешный MQTT connect не квалифицируют physical device.
   Неавторизованный запрос не получает доступ к receipt по idempotency key.
2. **Неизменный intent.** Один command UUID и correlation UUID, actor, house/device,
   requested_on, C/E, execution profile, authority epoch, exact UTF-8 bytes и digest.
   HTTP не задаёт epoch, deadline, timing bounds, nonce или признак qualification.
3. **Durable before network.** Parent ledger и command_control фиксируются атомарно
   до публикации. Нет orphan child, legacy-only parent или пересериализации candidate.
   `stage` фиксирует dispatch_started_at до I/O, поэтому неопределённая публикация
   никогда не разрешает повторный stage.
4. **Один bounded permit attempt.** Durable CAS binding связывает command с
   house/device/epoch/boot/session/nonce и фиксируется до reply. Повтор exact request
   может вернуть только исходные bytes исходного ещё действующего grant; иной
   контекст, потеря cache или restart не перевыдаёт бюджет.
5. **Deadline не продлевается.** Повтор HTTP, MQTT retransmission, queue delay,
   reconnect, restart, outbox retry, ACK/state не изменяют C/E, m0 или remaining_us.
6. **No automatic replay.** Нет retry worker, outbox replay physical candidate или
   восстановления RAM window из сохранённого wall clock. Новый user intent после
   unknown outcome — отдельное осознанное действие, не автоматический retry.
7. **Evidence не является разрешением.** ACK accepted, PUBACK, HTTP 202, retained
   state, status online и `reported_on` по отдельности не означают applied.
8. **Fail closed != гарантированный OFF.** Отказ admission запрещает новую
   command-driven запись. Он сам по себе не меняет уже выставленный GPIO и не
   доказывает размыкание контактов. См. раздел 7.

## 3. Предлагаемый HTTP/service контракт (пока не реализован)

Сохранить публичный POST `/v1/devices/{device_id}/commands`, строгий boolean `on`
и обязательный Idempotency-Key. Привязка physical/software определяется trusted
inventory, не параметром запроса. Software branch остаётся прежним.

Порядок physical branch, без открытия существующего gate:

1. Проверить authentication, request schema, RBAC и актуальную house/device связь.
2. В одной SQL session взять locks по единому порядку: inventory/binding → device
   → command rows. Этот же порядок нужен deactivation/rebind/revocation коду;
   deadlock/concurrency tests обязательны до реализации, текущая согласованность
   всех writers ещё не доказана.
3. Найти `(house_id, requested_by, idempotency_hash)`. Тот же device/on возвращает
   исходный receipt/IDs без prepare/stage/publish; другой intent → 409. Это верно и
   для terminal/unknown outcome. Если clock не позволяет безопасно вычислить
   lifecycle, вернуть 503, а не заново создать/исполнить команду.
4. Для нового intent: active binding, поддерживаемый profile, closed physical gate,
   qualified healthy clock/recovery, online/readiness. Пока gate закрыт — 409,
   ноль новых rows и публикаций, как в PR #9. Timing/recovery unavailable → 503.
5. Проверить pending command; завершать его timeout можно только по исходному
   authority timing. Неопределённость старой epoch блокирует новый intent.
6. Из authority clock получить C и установить E=C+30s (либо меньшую отдельно
   утверждённую server policy, никогда больше 30s). Сконструировать transient row
   без dispatch_started_at. Вызвать `authority.prepare(session,row)` до любого I/O.
   Его текущий repo.save сам COMMIT: нельзя добавить внешний «атомарный» wrapper,
   который на деле коммитится раньше. Locks должны держаться до этого commit.
7. Unique-конфликт конкурентных одинаковых HTTP запросов: rollback, новая
   транзакция и повторное чтение оригинального receipt после authorization;
   повторного prepare/stage нет. Конфликт другого intent/pending → 409.
8. Только создатель нового intent вызывает `send_control_command`; stage повторно
   проверяет admission/identity/deadline. Отсутствие window после сбоя → отказ,
   а не reconstruction. Бounded publish timeout не разрешает повторную отправку.
9. После MQTT await перечитать locked row: не затереть уже доставленные ACK/state
   поздним `dispatched`. Receipt отражает durable evidence, HTTP 202 — принятие
   intent, не завершение действия. GET использует те же authorization/clock правила.

Новые HTTP statuses или lifecycle states этим проектом не вводятся. Существующее
`failed/publish_unconfirmed` означает **возможную доставку с неизвестным исходом**,
а не доказанное отсутствие GPIO. Cancellation до stage тоже не превращается в
автоматическую публикацию при повторе. Детальную таблицу ошибок submit_control
нужно утвердить отдельно; нельзя маскировать unavailable как success.

## 4. Deadline и временные предпосылки

C/E — UTC microseconds в неизменном candidate; `0 < E-C <= 30_000_000`.
Authority хранит RAM monotonic deadline исходной epoch. В request вычисляет:
`R = min(2_000_000, original_deadline_tick - current_tick) - 1`.
R должен быть положительным; stage/reply проходят последнюю validate_delivery
перед publish. Задержка сокета после проверки не продлевает разрешение на устройстве.

Worker фиксирует m0 при candidate до первой request enqueue; grant обязан совпасть
по command/digest/epoch/boot/session/nonce/E. SHA-256 здесь не подпись: предпосылка —
verified TLS, отдельные credentials, точные directional ACL и один authority leader.
Скомпрометированный разрешённый principal не устранён nonce/digest.

При d = monotonic_now_us - m0 допускается GPIO только если:

```text
ceil(d * (1_000_000 + core_ppm) / (1_000_000 - device_ppm))
  + epsilon_us
  + ceil(gpio_us * (1_000_000 + core_ppm) / 1_000_000)
  < R
```

Равенство запрещает запись. Clock/bounds должны быть qualified, без rollback или
необнаруженного suspend; admitted/generation должны оставаться действительны.
Повторная проверка обязательна после NVS commit и непосредственно перед GPIO.
Граница gpio_us включает весь final-check-to-write путь. Примерные host bounds
не становятся production значениями; sample/read/lock/IRQ/cache latency на плате
не измерена. Проверка generation и write должны иметь доказанную синхронизацию с
revocation: нынешний portMUX сам по себе не доказывает её для MQTT callbacks.

## 5. Replay, отзыв и restart — точные гарантии

| Событие | Обязательное поведение | Что нельзя обещать |
|---|---|---|
| HTTP retry с тем же key | Исходный receipt без второй публикации | Новая попытка после 500/timeout |
| MQTT duplicate candidate/reply | Exact NVS identity может вернуть прежний outcome; нового GPIO нет | «Exactly once» при crash вокруг GPIO |
| Новый nonce/boot/session для consumed command | Отказ; budget не переиздаётся | Возобновление старой команды после reconnect |
| Core revoke/deactivate до permit | Повторная server admission проверка блокирует новый permit | Мгновенная остановка уже выданного grant |
| Revoke после grant | Уже выданный grant остаётся ограничен исходным R/E | Дистанционный мгновенный revoke или автоматический OFF |
| Device disconnect/generation/time fault | Отмена ожидания; старый reply не принимается | Атомарность callback/write без отдельного concurrency proof |
| Core restart | Новая epoch, пустые RAM windows/replies, recovery snapshot/fence; старые intent не adopted | Восстановление срока из только wall UTC |
| Device reboot | Новый boot/session; GPIO boot OFF policy; pending NVS → failed, без re-execution | Доказательство контактов OFF или того, что до crash GPIO не менялся |
| Full/corrupt NVS | Нет новых записей GPIO, нет auto-erase для «восстановления» | Сохранение availability ценой потери replay history |

Для remote revoke wire v1 **не имеет cancel handshake**. Contract этого этапа —
остановить выдачу новых разрешений и дождаться bounded validity уже выданных.
Гонка server authorization/commit/publish должна иметь указанную linearization
point и совместные locks либо admission version; текущий код нельзя объявлять
линеаризуемым без тестов. Требование более сильного отзыва — отдельный protocol
change и safety mechanism, не флаг `cancelled` в HTTP ledger.

Core recovery fence начинается после snapshot unresolved old-epoch rows:
`ceil((LIFETIME_US + recovery_error_us) * (1e6 + ppm) / (1e6 - ppm))`.
Это условная гарантия при квалифицированных обоих таймерах и исключении старого
leader. Unqualified clock не «выздоравливает» простым sleep. Старый authority,
продолжающий выдавать новые разрешения параллельно, fence не нейтрализует:
нужна отдельно доказанная single-leader/fencing архитектура.

## 6. ACK/state, crash windows и UI-смысл результата

Core подтверждает applied только при живых, корректно связанных house/device/
command/correlation evidence, durable permit binding, ACK applied и state.on,
равном исходному requested_on, до исходного deadline. ACK/state допускают разный
порядок; invalid/foreign/retained evidence не завершает command. Negative ACK
терминален; поздние ACK/state не оживляют timed_out/failed/applied receipt.
Текущий device state и исторический receipt — разные сущности: replay может
сообщить текущее состояние после более поздней команды, не доказав новое исполнение.

| Момент сбоя | Durable знание | Безопасное продолжение |
|---|---|---|
| До prepare COMMIT | Нет полного intent | Ноль публикаций; новый запрос допустим по обычным rules |
| После prepare COMMIT, до stage | Intent есть, delivery не доказана | Retry читает receipt; не инициирует stage; исход истекает/проходит recovery |
| После stage COMMIT, publish cancellation | Attempt помечен, доставка неизвестна | Нет replay; сохранить unknown semantics, ждать evidence/expiry |
| После permit binding COMMIT, до reply | Attempt consumed | Cache loss не выдаёт новый permit; дождаться timeout |
| До durable NVS pending | Исполнение ещё не разрешено | Ноль GPIO; storage failure закрывает admission |
| После pending, до GPIO | Действие могло не произойти | При reboot failed, без повторного GPIO |
| После GPIO, до terminal NVS commit | Физический исход неизвестен для durable history | Не посылать applied ACK; latch storage fault; reboot без re-execution |
| После terminal commit, до ACK/state delivery | Outcome durable на устройстве | Evidence retry допустим; Core после E не меняет terminal receipt |

`reported_on` — программное сообщение драйвера, не измерение контактов. Даже
applied означает выполнение программного контракта, не электрическую гарантию.

## 7. Безопасное состояние выхода

Существующий `relay_init` сначала устанавливает inactive level и только затем
настраивает output; initialization failure не разрешает команды. Необходимы
внешние электрические bias/interlocks для reset/boot до кода. Active-low и
active-high проверяются отдельно; «низкий уровень» не универсален для OFF.

Текущий deadline ограничивает момент **command-driven write**, а не длительность
ON. При утрате связи, истечении E, revoke или ACK loss код не гарантирует OFF.
Для новой неизвестной нагрузки нельзя автоматически считать OFF безопасным:
policy по оборудованию должна задать safe state, удержание/снятие питания,
watchdog и поведение при неисправном реле. Emergency OFF не должен зависеть от
доступности Core/MQTT или успешной доставки обычной OFF-команды.

До определения load-specific policy и аппаратного подтверждения внешний вывод:
«новая actuation запрещена; реальное состояние может быть неизвестно». Не
показывать пользователю «безопасно выключено» по HTTP timeout/rejected ACK.

## 8. Почему здесь нет положительного actual-worker теста

Текущий worker создаёт budget как `qualified=false` внутри `command()`.
Положительный PR #7 тест проходит parser/control/journal/engine с лабораторным
budget отдельно от worker. PR #9 исполняет сам worker и получает настоящий rejected
ACK. Подмена control_reply, kz_budget_valid или generated source с заменой false
проверяла бы изменённую политику, а не изолированные clock/GPIO границы.

Пока нет выделенного dependency boundary admission policy, production build proof
и проверки обоих ESP-IDF targets. Поэтому этот PR **не добавляет test enable macro,
сетевой флаг, weak symbol, runtime setter или environment override**. Отсутствие
положительного теста здесь намеренное; его безопасность не доказана, не объявлена
принципиально невозможной.

Критерии отдельного будущего test-only boundary review:

- Production entry point безусловно выбирает closed policy, никакой MQTT/HTTP/NVS/
  local config возможности переключения. Test driver находится только в `tests/`.
- Положительный harness использует тот же worker/reporting код; не заменяет parser,
  control_reply, journal, engine, admitted check или ACK/state. Подменены только
  явно описанные policy/SDK time/GPIO/NVS зависимости; qualification называется
  лабораторной и не считается production admission.
- Отдельный host-only target. Попытки собрать test policy в ESP-IDF/ESP_PLATFORM
  должны завершаться build error, даже при ручной передаче test definitions.
  Source grep и один `#ifdef` не являются достаточным isolation proof.
- Независимая CI matrix: ESP32-WROOM и ESP32-C6 production build, отрицательные
  compile tests с test definitions, link map/ELF symbols и component source list
  без test provider, неизменный закрытый worker regression в production variant.
- SDK doubles не допускают реального GPIO/network credential доступа. Реальный
  broker использует только fixture TLS/ACL; lab permit не является attestation.
- Positive ON/OFF дополняется delayed NVS, exact deadline boundary, rollback,
  generation/revoke race, outbox backpressure и no-applied-on-commit-failure.
  Текущий bounded-tick harness перезапускает локальные переменные worker при tick;
  для длительного scheduler/outbox теста нужна сохранённая lifetime worker task.

Без выполнения этих критериев положительный actual-worker PASS не заявлять.
Аппаратную WCET/power-cut/contacts qualification host matrix всё равно не заменяет.

## 9. Последовательность следующих PR и exit criteria

| Шаг | Изменение | Условие завершения |
|---|---|---|
| A | Согласовать этот контракт, unknown outcome и load safe-state policy | Устранены неоднозначности revoke, failures, leader и пользовательских сообщений |
| B | Core service `submit_control`, production gate по-прежнему закрыт | Isolated PostgreSQL race/rollback tests, exact persisted bytes, no duplicate stage, HTTP 409 production regression |
| C | Host-only policy boundary после отдельного design review | Все build/isolation criteria раздела 8; production variant только rejected |
| D | Положительный lab HTTP→control→worker integration | Один durable intent, настоящий C ACK/state, ON/OFF и весь negative/crash matrix, чёткая lab-only маркировка |
| E | Qualification и safety review вне этого задания | Доказанные clocks, leader exclusion, revocation synchronization, WCET/NVS/reset и load-specific hardware safety |

Шаг D не открывает production gate. Шаг E не авторизован этим документом.
Никаких автоматических merge/deploy/flash по результатам CI.

Минимальный matrix для B–D: одинаковые и конфликтующие idempotency keys в двух
процессах; concurrent pending; cancellation в каждом commit/I/O окне; ACK-before-
publish-return; expiry на равенстве; replay до/после cache loss/NVS reclaim;
Core restart с неверным UTC/suspend; device reboot с pending; revoke до/после
binding; malformed/retained/foreign packets; late и opposite-state evidence.
Каждый тест проверяет ledger, count публикаций, exact IDs/bytes и GPIO count,
а не только HTTP status или строку PASS. Не все эти combinations уже покрыты.

## 10. Проверка этого документа и оставшиеся пробелы

Сопоставлено с текущими ControlAuthority/ControlClock/CommandService,
`device_mqtt.c`, `control.c`, `journal.c`, `command_engine.c`, `relay.c` и CMake.
Runtime и существующие тесты не изменены. Наследуемый service CI на точном head
этого PR должен подтвердить отсутствие регрессий; он не исполняет proposed
submit_control, не доказывает test-policy isolation и не квалифицирует hardware.

Незакрытые вопросы: единый lock order всех binding writers; grant/revoke
linearization; устойчивый leader fencing; recovery under suspend; production
clock bounds; final-check/write synchronization и WCET; load-specific safe state;
реальная NVS power-cut durability; host-only policy build/link isolation. До их
решения сохраняются оба production gates и запрет обещать полный physical E2E.
