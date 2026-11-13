# Test-only compatibility with Core #37

Firmware source baseline: #12 at 4a3bde0fb14c81e5c9b0230bab44a95c099ffa12.
Both host and service CI now pin Core #37 at
a2bc83aa2451590f71ac34f9aad1c7c328bccb19.
This supersedes the Core #36 pin only in this compatibility branch.
Original #12 and main remain unchanged.

Core #37 descends from #36 and changes scene preflight and test diagnostics.
Its comparison against #36 changes six files; no transport, wire schemas,
requirements, authentication runtime or physical qualification changes.
Tests here must execute afresh; prior #36 results are not #37 evidence.

Run the unchanged host suite, disposable PostgreSQL/Mosquitto TLS service
scenarios and full pinned Core pytest, plus ESP-IDF WROOM/C6 builds.
Positive software Relay and lab C engine tests remain separate from the
production C worker's rejected ACK / zero GPIO path.

No firmware runtime changes, secrets, permissions, hardware or deploy.
Newer Web changes and confirmed per-action scene execution are not covered
by this Firmware task. Historical JWT 401/401 root cause is not claimed fixed.
Exact fresh CI outcomes are recorded in the draft PR description.
