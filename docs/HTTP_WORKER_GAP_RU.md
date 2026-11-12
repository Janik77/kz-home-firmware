# HTTP API и настоящий MQTT worker: границы интеграции без платы

## Зависимости и воспроизведение

Эта ветка основана на Firmware PR #7 (`codex/service-e2e-on-off`), commit
`75bb97dc088426aee9984ca8b14680ebb62903f8`. Draft PR направлен в эту ветку,
чтобы показывать только следующий этап; после её merge понадобится retarget на main.
Core остаётся на PR #32 (`codex/core-ci`),
`159eb98f2349b1aa2b8afd313c3a3f1fbf9b6bd2`. Runtime Core не меняется.

Существующий workflow **Core MQTT firmware service E2E** запускает весь набор:

```sh
python tests/run_host_tests.py --cc gcc --core-root /absolute/path/to/core \
  --services --full-core-services
```

Нужны Core requirements-dev.txt, Python 3.12, GCC, CMake, OpenSSL и Docker.
Изоляция и cleanup прежние: собственные PostgreSQL tmpfs, MQTT TLS/ACL,
loopback ports, одноразовые пароли/CA, никаких production volumes или secrets.
HTTP использует отдельный случайный loopback port и закрывается в finally.

## Добавленное доказательство

1. Полный Core `create_app`, Uvicorn и настоящий TCP HTTP client (не ASGITransport).
   Настоящие login/JWT/RBAC, command POST/GET, PostgreSQL и MQTTGateway.
   Для отдельной **software identity** существующий `simulator.mqtt_relay.Relay`
   возвращает ACK/state по broker: ON, OFF, обе evidence timestamps, applied,
   idempotent HTTP retry без второй MQTT команды. Anonymous POST получает 401.
2. Аутентифицированный owner отправляет ON/OFF для **physical identity**:
   HTTP 409 с точным expiry-contract отказом, количество ledger rows неизменно,
   наблюдатель broker не получает control candidate. Никаких dependency overrides,
   переименования physical в software или monkeypatch gates.
3. Production `device_mqtt.c` callbacks/worker, parser, control, journal и engine
   исполняются в host C процессе. `worker_service_bridge.c` переиспользует
   `test_mqtt.c` SDK/queue doubles через include, как существующие тесты включают
   production worker. Единственное расширение harness — observer публикаций.
   Python переносит **выданные C worker payload bytes, QoS и retain** в реальный
   Mosquitto. Он не создаёт ACK/state и не превращает rejection в applied.
   Broker candidate → C worker request → lab Core grant → C worker rejected ACK
   → Core ledger failed/device_rejected; ноль GPIO, повтор reply не исполняется.

## Почему единого положительного HTTP → настоящий worker → GPIO нет

Это проверяемые ограничения исходного кода, а не отсутствие эмулятора:

- `app/api/commands.py` вызывает `CommandService.submit`, который до создания
  ledger вызывает `PhysicalBindingGuard.require_command_contract`.
  `app/services/onboarding_service.py` безусловно отвергает зарегистрированную
  physical identity: `Physical command expiry contract is not verified`.
- `CommandService.submit` использует legacy `transport.send_command`, а не
  authority.prepare/send_control_command. Снятие одного guard не создаст
  правильный control-permit HTTP путь.
- `app/main.py` создаёт `ControlAuthority()` с закрытым gate и unqualified clock.
  Service fixture положительных проверок использует отдельную lab authority;
  это не production HTTP wiring.
- `kz_relay/main/device_mqtt.c`, `command()` жёстко задаёт
  `kz_budget budget = {.qualified = false, .admitted = still_admitted};`.
  Даже корректный lab grant не даёт положительного исполнения. Новый тест
  сохраняет это поведение и проверяет настоящий rejected ACK на broker.

Заявлять положительное исполнение actual worker, подменив qualified, ACK,
CommandService или physical classification, было бы неверно. Здесь это не делается.

## Оставшиеся границы и следующий обоснованный этап

- ESP-MQTT network stack, FreeRTOS scheduling, GPIO/NVS/time остаются host doubles.
  Реальные CONNECT/SUBACK выполняет Python transport до SDK event translation;
  это не запуск ESP-IDF firmware image и не проверка RTOS concurrency.
- Положительный C engine цикл из PR #7 остаётся отдельно lab-only. Положительный
  HTTP software цикл не является доказательством физического firmware исполнения.
- Сначала отдельно спроектировать HTTP control submission с durable authority
  preparation, idempotency и закрытым production admission. Проверить rollback,
  retry и timeout, не включая physical gate.
- Для положительного worker теста требуется отдельно одобренная архитектура
  внедрения test-only qualified budget на SDK boundary, с compile-time isolation
  и доказательством отсутствия пути включения в production. В этом PR такого
  изменения нет; переименование результатов не заменяет это решение.
- Production qualification по-прежнему требует доказательств clock bounds,
  GPIO latency, NVS/power-cut и аппаратной приёмки. Без платы эти свойства
  не подтверждаются. Merge/deploy/physical enablement не выполнялись.

## Статус

Полный локальный host runner и компиляция worker bridge прошли, Ruff прошёл.
Локально Docker отсутствует. Результат настоящих HTTP/broker/DB проверок следует
проверять в CI на точном head SHA draft PR; исторический успех PR #7 не является
доказательством новых сценариев. Десять прежних opt-in skips не считаются PASS.
