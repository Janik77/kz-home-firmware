# Host CI и совместимость с Core

Workflow `.github/workflows/host-tests.yml` запускает существующий
`tests/run_host_tests.py` на GitHub-hosted Ubuntu 24.04, Python 3.12, GCC,
CMake и OpenSSL. Он выполняется для pull request (включая draft), push в main
и ручного workflow_dispatch. Нужны доступные runner minutes и разрешение Actions.

## Закреплённый baseline

Core: `Janik77/kz-home-core` commit
`2359807c2781ce956ed43bc603772b66dd27955b`
(Provisioning v0.1.1 part 1, PR #31). Это проверенная совместимость с конкретным
snapshot, а не обещание совместимости с будущим Core main. Checkout проверяет
полный SHA. Core requirements дополняет `tests/ci-core-constraints.txt` с точными
версиями транзитивных runtime-зависимостей для Python 3.12/Linux; pip принимает
только wheels. При обновлении Core изменяйте SHA и constraints отдельным
reviewable изменением и повторяйте всю suite. Версия checkout action тоже
закреплена полным SHA. Образ Ubuntu и его системные пакеты обновляются GitHub:
это воспроизводимая процедура, не побитово герметичная сборка. Версии инструментов
печатаются в логе; wheel hashes пока не закреплены.

## Что проверяется

- Production C parser, journal, command engine и control с fake NVS/clock/GPIO.
- Пять сценариев настоящего Core authority ↔ C firmware через pipes:
  apply, expiry, bad digest, delayed reply и delayed ACK.
- Обе host MQTT worker/status suites и приём их сообщений настоящими Core
  schema/gateway; восемь вариантов GPIO/полярности, 31 compile guard,
  CMake board selection и изоляция конфигураций.

Workflow всегда передаёт `--core-root`: совместимость не остаётся намеренно
пропущенной, как при standalone запуске без этого параметра. Любой ненулевой
код теста завершает job ошибкой. SQLite и fake MQTT используются только локально.

## Граница доверия

Используется `pull_request`, **не** `pull_request_target` или привилегированный
`workflow_run`. Код PR выполняется на одноразовом GitHub-hosted runner с
`contents: read`, без секретов, environment, self-hosted runner, deploy,
публикации артефактов или shared cache. Оба checkout имеют
`persist-credentials: false`. Не добавляйте PAT или write permissions ради
доступа к Core: этот baseline публичный. Запуск недоверенного PR требует
сохранения этих ограничений; изменение самого workflow подлежит review.

## Повторить локально (без платы)

Нужны Python 3.12 с venv, GCC, CMake, OpenSSL headers/library (например,
`libssl-dev` на Ubuntu). Core должен быть отдельным checkout с указанным SHA.
Из корня Firmware:

```sh
python3 -m venv /tmp/kz-firmware-ci-venv
/tmp/kz-firmware-ci-venv/bin/python -m pip install --only-binary=:all: \
  -r ../kz-home-core/requirements.txt -c tests/ci-core-constraints.txt
/tmp/kz-firmware-ci-venv/bin/python -m pip check
git -C ../kz-home-core rev-parse HEAD
/tmp/kz-firmware-ci-venv/bin/python tests/run_host_tests.py \
  --cc gcc --core-root "$(realpath ../kz-home-core)"
```

## Что зелёный CI не доказывает

Это не ESP-IDF build/link для ESP32/ESP32-C6 и не проверка target toolchain,
SDK, flash partitions или реальных TLS/Wi-Fi/SNTP. Fake MQTT и pipes не заменяют
MQTT E2E с Mosquitto TLS, PostgreSQL и реальным transport Core. Опциональные
`--services`/`--full-core-services` здесь не запускаются. Нужны отдельные сборки
ESP-IDF и service E2E; затем аппаратные проверки timing, NVS, reconnect,
GPIO/полярности и поведения при отключении питания.

Production `qualified=false` и отказ от legacy `/set` остаются без изменений.
Положительные сценарии используют только тестовую квалифицированную policy;
они не разрешают физические команды, commissioning, flash или deployment.
