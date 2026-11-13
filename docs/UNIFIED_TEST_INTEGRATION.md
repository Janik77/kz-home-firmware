# Unified Firmware test integration (2026-11-13)

TEST ONLY: do not merge, deploy, or enable physical commands.

Firmware starts at PR #11, commit 0117b7e3a91171dc1fd31298ed1aed8ec3c1fe83,
which includes current #6/#8/#7/#9/#10. Those heads and Firmware main were
unchanged at audit time; the README conflict was already fixed in #8.
Original PRs and #11 remain preserved.

## Exact Core dependency

Both host and service workflows now pin Core #36:
6783ce0347004d101ec2ffa5eb60c48946901786
https://github.com/Janik77/kz-home-core/pull/36

Verified through commit comparison: this descends from #35
9a6283c260c92acfe0447b0c425668cbb1a64002 (which includes #32); its
app/transports/mqtt_client.py blob exactly matches #33
bbb9d7af8647ecc727b9c8201a936f66fa99a91e. It also includes #34 real-broker
tests and the unified software stack launcher. requirements.txt is byte-identical
to #32, so existing host constraints remain applicable. No runtime/contract
change is introduced in Firmware and no temporary overlay remains in CI.

Core #36 pins its Web software fixture separately. Firmware service tests use
the actual pinned Core application, existing simulator.mqtt_relay.Relay,
PostgreSQL and Mosquitto TLS. Web browser validation belongs to Core/Web CI,
not this Firmware workflow.

## Validation boundary

Run host compatibility, full disposable PostgreSQL/Mosquitto TLS + software
Relay HTTP ON/OFF/ACK/state cycle and full Core tests, plus real ESP-IDF
WROOM/C6 builds. Existing Core broker opt-ins may be skipped by the Firmware
service runner unless their dedicated broker fixture is provided; report skips
rather than claiming those tests passed. Core's dedicated real-broker CI covers
that separate fixture.

No new permissions, secrets, production endpoints, hardware or positive physical
admission. qualified=false and all physical/legacy gates are unchanged.
The ESP-IDF images have no local credentials/CA; passing builds does not qualify
Wi-Fi/RTOS timing/NVS/GPIO or hardware.

A temporary predecessor tested #35 plus the three #33 paths:
Core tree 848414b555452205a9cf57a1651bcbff92fb4e1e.
Firmware db37c954994a83ea0b90c6ac627480d9b83c0701 passed host, both target
builds and service CI (742 passed, 10 skipped). Those results do not replace
the new exact-Core-pin CI results recorded in PR #12.

Local execution was unavailable (exec-server transport disconnected). Changes
were prepared using GitHub APIs; actual builds/tests execute in Actions.
