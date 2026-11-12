# Воспроизводимый Core ↔ MQTT ↔ firmware C host цикл

База: Firmware `c9ba186907fec6bf6237f4ce811272743ece0334`, Core
`159eb98f2349b1aa2b8afd313c3a3f1fbf9b6bd2` из `codex/core-ci`
([Core PR #32](https://github.com/Janik77/kz-home-core/pull/32)).
Это точная зависимость для полного Core pytest: PR #32 исправляет устаревшие
migration assertions 0007 → 0008, не меняя Core runtime. Workflow получает этот
commit напрямую, merge для запуска не нужен. Firmware PR #6 не требуется.
После принятия Core PR #32 pin можно отдельно обновить на проверенный main SHA.

## Запуск

GitHub Actions: workflow **Core MQTT firmware service E2E**, на pull_request
или workflow_dispatch. Ubuntu 24.04, Python 3.12, GCC, CMake, OpenSSL, Docker.
Checkout Core закреплён на SHA; GitHub token имеет только contents:read,
checkout не сохраняет credentials. Repository secrets и deployment environments
не используются. Сервисы создаёт существующий `tests/service_integration.py`
на одноразовом runner, а не production compose. Это сохраняет один и тот же
локальный и CI сценарий, включая TLS, ACL и cleanup.

Локально, из firmware checkout, с доступным Docker:

```sh
python3 -m venv /tmp/kz-service-venv
/tmp/kz-service-venv/bin/python -m pip install -r /absolute/path/to/core/requirements-dev.txt
/tmp/kz-service-venv/bin/python tests/run_host_tests.py --cc gcc \
  --core-root /absolute/path/to/core --services --full-core-services
```

PostgreSQL и Mosquitto получают уникальные имена, случайные loopback ports,
новую tmpfs DB, временные CA/пароли и ACL. Каталог fixture доступен для traversal
контейнерному Mosquitto UID (Linux); production файлов в нём нет. Cleanup удаляет
только ресурсы с собственным label. Не сохраняйте TEMP каталог в CI artifacts.
Образы пока закреплены на major tags (`postgres:17-bookworm`, `eclipse-mosquitto:2`,
`alpine/openssl:latest`), transitive Python зависимости не lock-файл: сценарий
воспроизводим, но окружение не является побитово воспроизводимым.

## Что проверяется

- Один C host процесс: ON, затем OFF; по одной попытке записи на команду,
  совпадение requested/reported state в PostgreSQL и состояния устройства Core.
- ACK с неверной correlation игнорируется. Правильный ACK без state оставляет
  dispatched. State с чужой correlation не становится evidence; связанный state
  с противоположным значением не завершает команду. Правильная пара даёт applied.
- Повтор reply не даёт дополнительной записи.
- Инъекция отказа recording GPIO: C engine возвращает failed, MQTT ACK переводит
  ledger в failed/device_failed; включения fake relay нет.
- Существующие expiry, delay, digest, replay, reconnect/reboot, restart fence,
  retained bootstrap, TLS/auth/ACL и PostgreSQL CAS/constraint проверки сохранены.
- Полный Core pytest запускается существующим --full-core-services; skipped тесты
  не считаются пройденными.

## Граница доказательства

Настоящие: Mosquitto TLS и PostgreSQL, Core gateway/authority/lifecycle,
production C parser/control/journal/command engine. Python adapter переносит
command/request/reply через broker и собирает ACK/state из результата C.
Это не исполнение ESP-MQTT worker через реальный broker: его SDK callbacks
проверяются отдельно host suite. GPIO, NVS и SDK time — doubles; положительный
control gate/clock разрешён только в fixture. HTTP API/Web здесь не запускаются.
Core simulator.Relay остаётся отдельной существующей software-only проверкой.
Нового эмулятора нет. Physical gate, runtime, оборудование и production не меняются.

## Статус проверки

Локальная среда разработки не имеет Docker/PostgreSQL/Mosquitto; поэтому service
E2E выполняется только в CI. Полный локальный host runner прошёл с CMake из
`/tmp/kz-firmware-tools/bin/cmake` (передан через `--cmake`).

Первый [CI run](https://github.com/Janik77/kz-home-firmware/actions/runs/43249957732)
на Firmware `46ebb1ae7777fede32920145e03c0ca6f6bc2cf4` и Core main `2359807c`
подтвердил ON/OFF, ACK/state, GPIO failure, все MQTT и PostgreSQL service сценарии,
recovery/CAS и cleanup. Полный Core pytest: **693 passed, 7 failed, 10 skipped**.
Все семь failures — прежние assertions migration head `0007` вместо `0008`.
Именно поэтому финальный CI pin использует исправленные tests из Core PR #32;
failures не исключены и не превращены в xfail. Исторический отчёт
`CONTROL_SERVICE_INTEGRATION_V0.22_V0.12_RU.md` описывает прежний запуск на другой
базе и не доказывает результат этого commit. Актуальное доказательство — успешный
run workflow на SHA draft PR; до такого run новые service проверки не подтверждены.
