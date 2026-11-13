# Unified Firmware test integration (2026-11-13)

TEST ONLY: do not merge, deploy, or enable physical commands.

Firmware starts at PR #11, commit 0117b7e3a91171dc1fd31298ed1aed8ec3c1fe83,
which includes current #6/#8/#7/#9/#10. No newer Firmware PR was found.

Temporary Core baseline, pending the final integration SHA:
- #35 at 9a6283c260c92acfe0447b0c425668cbb1a64002, including #32.
- Overlay exactly the three non-overlapping #33 paths from bbb9d7af8647ecc727b9c8201a936f66fa99a91e:
  app/transports/mqtt_client.py, tests/test_mqtt_lifecycle.py,
  docs/MQTT_SUBACK_VALIDATION.md.
- Both host and service jobs verify the base and fetched commit, apply the same
  reviewed overlay, and print TEMPORARY_CORE_TREE. This is a synthetic tree,
  not a claim that the #35 SHA alone contains #33.
- Core requirements.txt is unchanged from the previously tested #32 baseline.
  Existing host dependency constraints remain in use.

The final integrated Core SHA must be checked for these changes before replacing
this temporary assembly. No command protocol or physical gate changes are made.

Run the existing host, disposable PostgreSQL/Mosquitto TLS + software Relay HTTP
cycle and full Core tests, plus ESP-IDF WROOM/C6 builds. No new permissions,
secrets, production endpoints or hardware. Existing opt-in skips remain reported;
passing software tests does not establish physical qualification.
