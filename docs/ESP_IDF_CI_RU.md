# ESP-IDF target CI без оборудования

`.github/workflows/esp-idf-build.yml` собирает **оба настоящих target**:
`esp32_wroom` → `esp32` (Xtensa) и `esp32c6` → `esp32c6` (RISC-V).
Matrix jobs независимы, fail-fast выключен. Используются существующие
`boards/select.cmake`, общие и board-specific sdkconfig defaults, partitions
и раздельные `dependencies.lock.esp32` / `dependencies.lock.esp32c6`.
Runtime-код и конфигурации не меняются.

## Закреплённые источники

- [Официальный образ Espressif](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32/api-guides/tools/idf-docker-image.html):
  `espressif/idf:v6.0.3`, immutable multi-platform digest
  `sha256:54278f2c01e6e759502e2f04c9103589c17528923426f4d63193490c7a08f555`
  (проверен через Docker Registry 2026-11-12).
- [ESP-IDF v6.0.3](https://github.com/espressif/esp-idf/tree/76f5dedd9950a3012fee8fb7d5586df21fc67802),
  commit `76f5dedd9950a3012fee8fb7d5586df21fc67802`; job проверяет HEAD SDK.
  Образ содержит toolchains, CMake/Ninja и Python environment.
- `actions/checkout` v4.2.2 закреплён на
  `11bd71901bbe5b1630ceea73d27597364c9af683`.
- ESP-MQTT 1.1.0 и component hash берутся из существующих lock-файлов;
  изменение lock-файлов сборкой приводит к ошибке CI.

Обновление SDK требует отдельного review изменения digest, SDK SHA и lock-файлов.
Это фиксирует SDK/toolchain и зависимости, но не обещает побитовую идентичность
бинарников: runner/kernel, timestamps и пути сборки могут различаться.
Registry и component service должны быть доступны.

## Что делает CI

На pull_request, push в main и ручном запуске CI выполняет `idf.py build` и
`idf.py size`, проверяет target в `project_description.json`, наличие непустых
app ELF/bin, bootloader и partition table. В логе остаются размеры и SHA-256 app.
При ошибке одной платы вторая продолжает сборку. Бинарники не публикуются и
автоматически никуда не прошиваются.

Fresh checkout не содержит `kz_relay/local`; job явно проверяет его отсутствие.
По существующей политике CMake это образ без credentials/CA и с выключенной
сетью, relay OFF. Production `qualified=false` и запрет legacy `/set` сохранены.
Успешная сборка не даёт разрешения physical control.

## Воспроизведение

Нужен Docker на Linux и чистый checkout без local config. Из корня Firmware:

```sh
for board in esp32_wroom esp32c6; do
  docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -e IDF_CCACHE_ENABLE=0 -e IDF_GIT_SAFE_DIR=/project \
    -v "$PWD:/project" -w /project/kz_relay \
    espressif/idf@sha256:54278f2c01e6e759502e2f04c9103589c17528923426f4d63193490c7a08f555 \
    idf.py -B "build/$board" -D "KZ_BOARD=$board" build || exit 1
done
```

Либо активируйте официальный ESP-IDF 6.0.3 и выполните те же команды `idf.py`
в `kz_relay`. Не используйте один build directory для разных плат.
Не нужны `set-target`, flash, monitor, erase-flash или подключение serial port.

## Границы доверия и проверки

Обычный `pull_request`, не `pull_request_target`; `contents: read`, checkout
без сохранения credentials. Одноразовый GitHub-hosted runner и официальный
job container без privileged mode, host Docker socket, устройств, secrets,
environments, shared cache, deploy или привилегированного follow-up.
Изменения workflow всё равно требуют review.

Target build не заменяет host regression, MQTT/PostgreSQL E2E, реальную
проверку Wi-Fi/TLS/SNTP, timing/NVS при power loss, GPIO/полярности и внешнего
boot OFF bias. Образы без local config не проверяют runtime подключение к брокеру.
Физическая квалификация и commissioning остаются отдельной закрытой стадией.
