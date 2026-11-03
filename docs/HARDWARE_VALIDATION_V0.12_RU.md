# План аппаратной проверки Firmware v0.12

Дата подготовки: 2026-11-03. Платы: ESP32-WROOM-32 и ESP32-C6, отдельно.
Основа: [control permit](CONTROL_PERMIT_V0.12.md) и
[service integration](CONTROL_SERVICE_INTEGRATION_V0.22_V0.12_RU.md), Core `e73180a`.
Это план, **аппаратная приёмка не выполнена**. В этой работе нет flashing/deploy.
Production physical gate остаётся закрыт: worker задаёт `qualified=false`;
test fixtures и их bounds не являются разрешением управления оборудованием.

## 1. Готовность и этапы

Текущие образы подходят только для будущего отдельно разрешённого исследования
boot OFF. Без local header/CA сеть отключена. С private configuration production
worker может обмениваться permit, но GPIO по команде всё равно запрещён.
Готовой ESP32-прошивки для положительного timing/NVS/GPIO E2E сейчас нет.

| Этап | Без физического реле | С низковольтной нагрузкой | Условие начала |
|---|---|---|---|
| A: осмотр, OFF, reset | Да, только измерительные щупы | Нет | Проверка схемы и отдельное разрешение на flash |
| B: сеть, denial/replay, reconnect | Да | Нет | Изолированные сервисы, отдельная lab identity и private config |
| C: timing и NVS fault injection | Да, исполнительный тракт физически разомкнут | Нет | Отдельно reviewed диагностическая прошивка/fixture |
| D: реле/контакты | Нет | Только 5 V DC LED | Отдельное разрешение после review A–C и схемы |

Этап C не должен включать production gate. Для исследования участка с GPIO write
потребуется отдельный измерительный harness, вызывающий исследуемый driver path
на ненагруженном GPIO, с явной маркировкой LAB ONLY и независимым от сети запуском.
Его реализация, review и сборка — будущая задача. Он не доказывает разрешённость
production path, пока различия кода и instrumentation не учтены. Нельзя просто
заменить `qualified=false` на true или перенести host bounds на плату.

## 2. Оборудование первого испытания (этап A)

1. Одна development board ESP32-WROOM-32 с доступным GPIO26 и одна ESP32-C6
   development board с доступным GPIO2; точные модель, ревизию, схему и маркировку
   flash записать до подключения. Профили не подтверждают разводку конкретной платы.
2. ПК с ESP-IDF 6.0.3, USB serial driver конкретной платы, терминалом и свободным
   USB-портом; два подходящих USB data cable (разъём по фактическим платам).
3. Лабораторный изолированный источник 5 V DC с ограничением тока, USB breakout
   с разрывом VBUS и выключателем. Для первого старта установить лимит 0.5 A,
   при ограничении тока остановить опыт и проверить питание по документации платы.
   Не подавать 5 V на 3V3/GPIO; не соединять два источника питания параллельно.
4. Мультиметр; осциллограф ≥2 каналов, ≥50 MHz, ≥100 MS/s, два щупа x10
   с короткой землёй. Для будущего timing удобнее 4 канала. Характеристики прибора
   здесь — требования к стенду, не квалифицированные timing bounds.
5. Breadboard, 10 проводов Dupont, два резистора 10 kΩ, два резистора 1 kΩ,
   один красный LED, кнопка разрыва низковольтного питания. LED в первом опыте
   остаётся отключённым. Для active-high поставить 10 kΩ GPIO→GND после проверки
   схемы; для отдельного active-low опыта нужен 10 kΩ GPIO→3V3.

Дополнительно для B–C: изолированная Wi-Fi AP/LAN, отдельные PostgreSQL/Mosquitto
TLS/Core lab services, операторский SNTP, индивидуальные lab credentials вне Git;
логический анализатор с входами 3.3 V, ≥4 каналов, ≥50 MS/s; управляемый разрыв
5 V (load switch с известным временем отключения), отдельный генератор/контроллер
для trigger и power-cut, независимая временная опора с паспортной погрешностью.
UART-адаптер при необходимости — только 3.3 V logic, VCC не подключать.

## 3. Безопасная схема

Первый опыт: источник → USB VBUS breakout → питание development board.
USB data к ПК, VBUS от ПК разорван. Общая земля платы и измерительных входов;
CH1 → выбранный GPIO, CH2 → 3V3 (затем EN отдельным прогоном). На GPIO только
щуп и внешний OFF bias. Реле, драйвер и нагрузка полностью отсоединены.
Убедиться, что UART/USB/GPIO не подпитывают выключенную плату: power-cut должен
фактически обесточивать 3V3, что подтверждается CH2.

Будущий этап D: отдельный заводской relay driver/module с документированным
входом 3.3 V, питанием катушки 5 V DC, защитой выброса и известной полярностью.
GPIO подключать только к логическому входу драйвера, не к катушке. Для
неизолированного драйвера земли объединить по его схеме; для оптоизолированного
сохранить предусмотренную производителем изоляцию. Не выбирать модуль с 5 V
pull-up на входе ESP32. Оборудование подбирается после проверки datasheet модуля.

Контактная цепь: отдельные 5 V DC → предохранитель 100 mA → COM;
NO → 1 kΩ → красный LED → возврат источника. Только эта низковольтная
резистивная нагрузка, без сетевых проводов и **без 220 V**. NC не подключать.
Отключение питания катушки должно размыкать NO. Измерить ток и напряжение
мультиметром до соединения с ESP32. Внешний bias должен удерживать OFF при reset
и обесточенном MCU; проверить отсутствие обратной подпитки.

## 4. Порядок A: GPIO и boot OFF, без реле

1. Сфотографировать маркировку, проверить схему, GPIO allowlist, flash capacity
   и partitions. Classic: кандидат GPIO26, C6: GPIO2; при конфликте остановиться.
   Не переносить назначение пинов с одной платы на другую.
2. Записать commit, board/target/GPIO/polarity, IDF version, sdkconfig и SHA-256
   ELF/BIN локально. Проверить отсутствие private config/CA для первого образа.
   Flash допускается только будущим отдельным действием после разрешения;
   этот документ не исполняет его. Существующий `commands` не стирать.
3. До питания проверить сопротивления/короткое замыкание и OFF bias.
   Подключить щупы, включить single-shot trigger по GPIO и питанию.
4. После разрешённой установки образа снять не менее 20 cold-start, 20 EN reset,
   20 software reset/watchdog recovery. Это screening sample, не доказательство
   вероятности отказа. Хранить полную осциллограмму от подачи питания до app init.
5. Ожидать inactive level до включения output и OFF после init. Любой active pulse
   записать с длительностью/амплитудой; не скрывать его программным cached state.
   Проверить медленный спад/рост питания в разрешённых пределах платы.
6. Повторить на второй плате. Active-low — отдельный образ/прогон с другим bias;
   проверяется только после анализа схемы, автоматически полярность не менять.

## 5. Порядок B: сеть и fail-closed, без реле

1. Отдельно подготовить lab identity и directional ACL, trusted CA/SAN, Wi-Fi/SNTP.
   Не использовать simulator identity, production DB/credentials или сеть наставника.
   Положительный Core test gate допускается лишь в изолированной fixture, не deployment.
2. После разрешённой установки lab-configured образа проверить TLS negative cases:
   wrong CA, hostname, password; отсутствие insecure fallback. Дождаться обоих
   SUBACK QoS 1, проверить command/reply/request topics и retain policy.
3. Доставить валидный grant: у production Firmware command-driven GPIO writes=0.
   Проверить legacy set, retained replay, чужой topic, bad digest, ID/nonce/session,
   unknown/duplicate keys, missing E, истёкший E и delayed reply ≥2 s: ноль writes.
4. Между request/reply отключить Wi-Fi, перезапустить lab broker, восстановить IP,
   выполнить MQTT takeover только lab identity. Старый grant не исполняется,
   session меняется; повтор команды не сбрасывает m0 существующего ожидания.
5. Перезагрузить ESP32: новый boot ID, выход OFF, старый permit не исполняется.
   Перезапуск Core даёт новую epoch/recovery fence, без перевыдачи старого grant.
   Отключение сети само по себе не является командой OFF и не гарантирует
   отключения уже включённой нагрузки в будущей реализации.

## 6. Порядок C: final-check → GPIO, без реле

Текущий production образ не достигает разрешённой записи; отсутствие фронта —
PASS закрытого gate, **не измерение latency**. Следующий порядок требует будущего
reviewed измерительного harness при физически отсоединённом драйвере.

1. Зафиксировать точку последнего `esp_timer_get_time()` в `relay_apply_permitted`
   и полный участок до завершения `gpio_set_level`. Сделать disassembly/map audit:
   timer, budget arithmetic, admission callback, network clock, critical section,
   IRAM/cache и библиотечные вызовы. Проверить допустимость вызовов внутри portMUX.
2. Инструментировать маркер до timer sample, второй после возврата GPIO write;
   CH1/CH2 — маркеры, CH3 — реальный выход GPIO, CH4 — power/trigger.
   Пины маркеров согласовать со схемой отдельно, без конфликтов с flash/USB/UART.
   Маркеры не должны открывать сетевой gate или попадать в release.
3. Измерить внешним прибором sample bracket→GPIO edge и bracket→driver return.
   Фронт выхода и возврат функции — разные события; учитывать обе границы,
   marker overhead, разрешение/погрешность прибора и влияние instrumentation.
   Одного UART timestamp или программного timer delta недостаточно.
4. Снять распределения и максимум минимум для 10 000 операций каждого режима:
   idle, интенсивный Wi-Fi/MQTT, заполнение outbox, NVS commit/GC, flash stalls,
   CPU/IRQ stress, частоты CPU и разрешённые режимы питания. Для WROOM нагрузить
   оба ядра, отдельно проверить межъядерные disconnect/generation/revocation races;
   на C6 проверить его scheduling/IRQ path. Сохранить dropped captures и resets.
5. Inject delay до/после NVS и перед final-check; проверить строгую границу
   `upper + epsilon + gpio < remaining`: равенство/превышение запрещают write.
   Сравнить независимую временную опору с monotonic timer, отдельно оценить drift
   и rate во всём заявленном диапазоне питания/температуры.
6. Максимум выборки не является WCET. До отдельного доказательства всех задержек,
   rate, suspend и revocation bounds не задавать production `gpio_us`, ppm,
   epsilon или `qualified=true`. Невоспроизводимая задержка сохраняет блокировку.

## 7. NVS power-cut и migration, без реле

Для положительного pending/GPIO пути нужен harness этапа C. На обычной production
прошивке можно проверить migration и записи rejection, но это не покрывает applied.

1. Использовать только expendable lab flash/history. Подготовить v1 записи исходным
   layout v0.11: terminal и pending; сохранить partition dump локально вне Git.
   Переход на v2 сохраняет IDs/binding/outcome; старый pending становится
   failed/hardware_failure, без повторной записи GPIO. Не делать erase для recovery.
2. Поставить trigger вокруг NVS reserve/commit, после pending commit, около GPIO,
   перед/внутри outcome commit, при migration каждого слота и при GC/full partition.
   Поэтапно и со случайным сдвигом разрывать питание; подтвердить падение 3V3
   осциллографом. Не подменять power-cut только EN reset.
3. Для каждой точки выполнить минимум 100 циклов screening, записать число попыток,
   успешно попавших в окно, dump до/после и результат восстановления. Недоступное
   окно явно пометить непроверенным. Потеря питания до durable pending: write=0;
   durable pending после reboot никогда не replay; сбой после write может означать
   unknown execution, без недостоверного ACK applied.
4. Отдельно проверить full journal, corrupt/unknown version, identity mismatch,
   ошибки NVS open/set/commit через fixture: управление закрыто, auto-format нет.
   Exact duplicate после reset возвращает исторический outcome и текущее state,
   изменённый payload того же ID не исполняется. Миграция должна быть возобновляемой
   после cut без утраты прежней истории. Downgrade не обходить стиранием журнала.

## 8. Suspend и clock faults

1. На lab устройствах сравнить внешний elapsed с `esp_timer_get_time` при light sleep,
   deep sleep, watchdog/debug halt и изменении CPU frequency; фиксировать причины
   пробуждения. Вход/выход sleep — только будущая fixture, не добавление runtime флага.
2. При deep sleep/reset проверить новый boot/session и OFF; при остановленном или
   откатившемся timer старое ожидание должно быть отменено. Даже если light sleep
   компенсирует timer, не считать suspend policy квалифицированной без измерения.
3. Отключить lab SNTP/health, подать wall jump вперёд/назад через fixture, в том числе
   между request и reply и около final-check. Не менять системные часы рабочей машины.
   Проверить cancel, network health и ноль command writes; UTC не является time-proof.
4. Suspend/resume изолированного Core host проверить отдельно с внешней опорой,
   recovery fence и single-authority exclusion. Текущие host injections не заменяют
   измерения реального suspend. Любая неизвестная continuity закрывает gate.

Справочные первоисточники IDF 6.0.3:
[GPIO ESP32](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32/api-reference/peripherals/gpio.html),
[ESP Timer C6](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32c6/api-reference/system/esp_timer.html).
Они описывают ограничения GPIO и поведение timer/sleep; разводку development board
подтверждает только схема именно выбранной платы.

## 9. Этап D и протокол результатов

После отдельного review A–C и разрешения на низковольтный опыт проверить OFF при
подаче питания/reset/brownout с драйвером, затем отдельно согласованный lab harness
для ON/OFF LED через NO. Измерить GPIO, вход драйвера и контакты; записать дребезг,
задержку срабатывания и отпускания. Они не входят автоматически в final-check→GPIO.
Не смешивать ACK applied с подтверждением контактов. Production gate не открывать.

Для каждого опыта сохранить: плату/ревизию/flash, схему/полярность/bias, commit и
hash образа, toolchain/sdkconfig, приборы и calibration/timebase, питание/температуру,
scenario/seed/ID, traces, UART без secrets, NVS snapshots в защищённом локальном
каталоге, expected/actual, число повторов, сбои и ограничения. Dump flash и бинарники
могут содержать credentials и не публикуются. Итог — отдельный отчёт с PASS/FAIL/
BLOCKED по обеим платам, без объявления production acceptance по одному успешному
опыту. Любой неожиданный фронт/повтор/утрата истории останавливает этап.
