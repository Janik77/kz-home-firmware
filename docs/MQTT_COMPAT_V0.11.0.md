# MQTT status compatibility and command safety — v0.11.0

## Audited baseline and scope

- Firmware main `ed6ebab` (v0.10.0), clean before branch creation.
- Core main **`6464bd8`**, v0.21; the requested `6464db8` transposes two characters.
- Web main `809266f`, v0.19.
- Branch: `feature/mqtt-compat-v0.11.0`. Only firmware is modified.

This change repairs status envelopes. It does **not** repair or redefine remote
command cancellation/deadlines. No physical acceptance, flashing, broker access,
production credentials, deployment changes or Core/Web edits are part of it.

## Verified compatibility contract

Read the actual Core files, not only DEVICE_PROTOCOL.md:

- `app/transports/mqtt_models.py`: StatusEnvelope requires `last_seen: datetime`;
  `status` is online/offline, optional protocol version is exactly v1, optional
  heartbeat interval is 30–300 seconds; `timestamp` is optional.
- `app/transports/mqtt.py`: status goes through identity admission and schema
  validation, then passes the supplied `last_seen` to the status handler. It does
  not replace that time with broker receipt time, including retained messages.
- `app/services/device_service.py`: availability and metadata.last_seen are
  updated from those values. Historical LWT can move last_seen backwards.
- Core v0.21 has `esp32-relay-v1` as well as `esp32-c6-relay-v1` onboarding profiles.
  Activation, exact identity/house binding and external per-device ACLs still apply.

The old firmware's `{"status":"offline"}` was rejected before availability
updates. Online already included last_seen and heartbeat metadata.

### Firmware fix

Before creating the MQTT client, validate UTC and construct a persistent LWT:

```json
{"status":"offline","last_seen":"2026-10-30T12:00:00.000Z","protocol_version":"v1"}
```

The example is synthetic. The actual value comes from synchronized device UTC.
It is a **startup-time observation**, not disconnect time or fresh broker receipt
time. It stays unchanged across this client's automatic reconnects. An older
retained offline message remains historical even when Core consumes it later.

Online and each 60-second heartbeat retain their existing fields and refresh a
worker-owned last-valid-time cache. On clock failure, offline uses that cache;
there is no call to an invalid wall clock to invent an event timestamp. Offline
omits the optional `timestamp` entirely. Invalid/missing initial time prevents
client creation rather than seeding an invalid LWT.

All firmware-controlled clean stops (clock fault and recovery restart) attempt
the valid offline payload before stop/DISCONNECT. There is no separate general
application shutdown API. Ordinary unexpected disconnects rely on the broker's
preconfigured will. Nothing is published from the disconnect callback.

QoS 1/retained true, topic, stable identity, server CA/name/time verification,
clean-session policy and subscription readiness are preserved. This is not a
new protocol version or relaxed Core schema.

Sending offline is best effort: a nonnegative SDK publish result is **not** a
broker PUBACK or Core receipt guarantee. ESP-MQTT can retain QoS 1 publications
in its outbox while disconnected. A clean stop can suppress LWT; a failed or
queued offline report can leave temporarily stale availability. Clock-fault
send failure retains the existing bounded retry behavior, with commands blocked.
Core heartbeat expiry/receipt-time availability remains a separate task.

## Command deadline investigation — unresolved deployment risk

Verified Core `app/services/command_service.py` behavior:

1. Persist command identity/intent and `expires_at = creation_time + 30 seconds`
   **before** network publication. Dispatch can therefore consume deadline budget.
2. Limit awaiting publication to five seconds. This neither recalls a delivered
   command nor cancels physical work.
3. Expire pending rows on read/submission/evidence processing; there is no implied
   independent device cancellation message. Once timed_out, late evidence cannot
   complete or regress that terminal lifecycle result.
4. `MQTTGateway.send_command` creates its timestamp at dispatch, not at lifecycle
   creation, and sends no `expires_at`, execute-before field or remaining budget.

Verified firmware behavior remains unchanged:

- Check first-delivery command age <=300 seconds and future skew <=30 seconds.
- Commit pending intent to NVS, then write GPIO, then commit terminal outcome.
- Suppress repeat execution by command_id; clean sessions/non-retained commands
  reduce replay exposure but do not impose an execution deadline.
- Recheck connection generation at dequeue, but a delayed valid command within
  the current connection can still execute. NVS commit and scheduling add delay
  between the existing freshness check and GPIO.

Concrete acceptance-test counterexample with aligned clocks:

```text
t=0s    Core records an ON intent with expiry t=30s and sends /set.
t=31s   A Core read/expiry check records timed_out.
t=60s   First delivery reaches firmware; age 60s is within 300s.
        Firmware can commit/apply ON. Core ignores late ACK/state for command
        completion; ordinary observed-state ingestion can still show ON.
```

The host C test executes the actual command engine at +60s and observes an
applied result/GPIO change. The companion actual-Core test expires a row at
+31s and confirms applied ACK/correlated state at +60s leave it timed_out.
These deterministic tests expose a risk; they do not claim a live network test.

### Why replacing 300 with 30 is insufficient

- **Clock skew:** firmware age uses device UTC; Core expiry uses Core UTC. The
  two-second discontinuity latch detects changes to a device clock anchor, not
  a guaranteed bound on device-vs-Core clock error. A slow device can underestimate
  age; a fast one can reject otherwise timely intent. SNTP is unauthenticated.
- **Core queueing/persistence:** wire timestamp is later than the persisted
  creation/deadline origin; 30 seconds from that timestamp is the wrong deadline.
- **Latency/queueing:** broker, TCP, worker queue, scheduling and NVS latency can
  consume budget. QoS 1 assures no exactly-once execution or cancellation.
- **Local receipt TTL:** a timer started on receipt ignores all pre-receipt delay.
- **Irreversibility:** Core timing out after an action cannot undo the GPIO write.

There is no positive firmware-only freshness constant that proves execution
before an unknown Core deadline under unbounded skew/delay. Disabling execution
would prevent the risk but break the existing device contract.

### Smallest safe policy and separate coordinated task

For v0.11.0, preserve the reviewed 300s freshness/30s future/600s dedup policies.
Do not represent timed_out as cancelled or definitely OFF. A timed-out command
has an uncertain physical outcome. Keep initial tests supervised and unloaded;
do not commission a real load under a requirement of “no execution after Core
timeout” until the coordinated deadline work is accepted.

Separate Core + firmware contract task, requiring explicit review:

1. Distinguish outcome-observation timeout from an authoritative physical
   execution deadline. Define the required guarantee and cancellation limits.
2. If no-late-actuation is required, convey the persisted authoritative
   execute-not-after value (or equivalent reviewed lease/budget), bound clock
   uncertainty and execution delay, and require devices to enforce it.
3. Gate enforcement by negotiated/registered support; old v1 firmware currently
   ignores unknown metadata, so adding an optional field alone proves nothing.
4. Check remaining safe budget immediately before GPIO, **after** NVS reserve;
   reject durably without actuation when expired or clock uncertainty is too high.
5. Specify retries/duplicate identity, terminal evidence, recovery and any
   physical completion bounds. Reuse the original intent deadline; do not reset
   it on retries. Test skew, delayed dispatch/delivery, NVS delay, reset and
   evidence arriving after the observation deadline.

This is a follow-up design, not an implemented field or a firmware-only protocol
extension. No lifecycle semantics, command freshness constants or NVS format
change in this patch.

## Acceptance and reproducible verification

Run from firmware with an installed host compiler and CMake. Core schema/gateway
verification explicitly reads the existing checkout/dependencies; it opens no
database, broker, Docker resource or network connection. Bytecode writes to Core
are disabled. No dependency installation is needed on the audited machine.

```powershell
.venv/Scripts/python.exe tests/run_host_tests.py --cmake C:/Espressif/tools/cmake/4.0.3/bin/cmake.exe --core-root C:/projects/kz-home-core --core-site-packages C:/projects/kz-home-core/.venv/Lib/site-packages --cc build/host-tools/ziglang/zig.exe cc
```

`--core-site-packages` must match the test interpreter ABI (here Python 3.12).
Without `--core-root`, actual-Core checks explicitly print SKIP, not PASS.

Required regression coverage:

- Both boards' actual MQTT callback/worker and real parser/engine/journal,
  replacing only SDK/NVS/GPIO/clock boundaries with deterministic fakes.
- Valid startup LWT, no client without valid time, online only after QoS 1 SUBACK,
  60s heartbeat, unexpected disconnect silence, reconnect/resubscribe, immutable
  historical LWT, graceful restart and cached-time clock-fault retry.
- Malformed generated timestamps do not publish online. Status-topic traffic and
  malformed commands do not actuate GPIO.
- Actual Core StatusEnvelope and MQTTGateway accept every captured firmware
  status on live and retained paths. Malformed JSON, absent/invalid last_seen,
  invalid status/version/heartbeat are rejected before status-handler mutation.
- Existing protocol/journal/board/polarity checks plus the late-command risk test.

Build each target separately in an activated ESP-IDF **6.0.3** shell:

```powershell
cd kz_relay
idf.py -B build/esp32_wroom-v011 -D KZ_BOARD=esp32_wroom build
idf.py -B build/esp32c6-v011 -D KZ_BOARD=esp32c6 build
```

Never reuse the old build directories or delete private configuration/artifacts.
Unconfigured builds validate compilation, not Wi-Fi/TLS/hardware acceptance.

Verified results (ESP-IDF 6.0.3, GCC 15.2.0, CMake 4.0.3, Ninja 1.12.1;
host Zig 0.14.1 cc):

| Check | Result |
|---|---|
| ESP32 fresh `build/esp32_wroom-v011` | ELF/BIN, bootloader, partition table; app 0xe0b10 bytes, 39% free |
| C6 fresh `build/esp32c6-v011` | ELF/BIN, bootloader, partition table; app 0x106b70 bytes, 29% free |
| Protocol/command/journal | 20,245 checks passed (20,242 existing + 3 late-command assertions) |
| Board/polarity/fault tests | Eight executable variants passed |
| Unsafe compile configurations | 31 rejected as expected |
| CMake isolation/selection | Three valid + seven invalid/environment cases passed |
| Actual MQTT worker | Both board profiles passed |
| Actual Core schema/gateway | Nine captured statuses per board; live and retained paths passed; seven malformed cases rejected per board |
| Python lint / C warnings | Ruff passed; host C compiled with -Wall -Wextra -Werror |
| Diff checks | git diff --check passed |

Both firmware builds omit local configuration/CA and disable networking. Existing
v0.10 target builds and private configuration were not cleaned or modified.
SDK BT/FATFS Kconfig notices were nonfatal. No live TLS/broker/power-loss or
physical relay acceptance is claimed. Core/Web working trees remain clean.

Release verification repeated on 2026-11-02: the full host runner (including
actual Core `6464bd8` schema/gateway checks) and both ESP-IDF 6.0.3 builds passed
with the sizes above. The staged file list and contents were reviewed for private
credentials, certificates and generated artifacts; only synthetic test fixtures
are included. Command deadline behavior remains unchanged.

Physical commissioning still requires verified board flash/layout, GPIO26 wiring,
relay logic/driver/power/bias and boot/reset waveform, approved Core activation,
unique principal/exact ACL, reachable private TLS/DNS/SAN/CA/time service, live
power-loss/LWT/reconnect tests and the appropriate command-deadline policy.
