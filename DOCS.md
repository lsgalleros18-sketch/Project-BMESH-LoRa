# Project Docs

This is the single source of truth for the ProjectLoRa codebase docs.

## What the code currently does

- ESP32-S3 ESP-IDF project with a PlatformIO build path.
- Captive portal served from LittleFS files in `data/`:
  - `data/index.html`
  - `data/setup.html`
  - `data/login.html`
- LoRa mesh messaging with encrypted `BEMS|...` packets.
- TX/RX, broadcast delivery, jittered forwarding, dedup suppression, ACK handling.
- NVS-backed node configuration and factory reset flow.
- NVS-backed message store with bounded retention.
- NVS-backed `highest_seen_id` persistence in `mesh_control`.
- NVS-backed packet counter persistence in `main.c`.
- Time sync now parses peer `epoch=` and `~dist=` payload data on receive.

## Verified module layout

- `src/main.c`
  - Boot orchestration
  - HTTP context wiring
  - Packet receive handling
  - Message queueing
  - Packet counter load/save
- `src/mesh_control.c`
  - Highest-seen packet ID load/save
  - Time sync send/receive helpers
  - ACK and sync response generation
- `src/mesh/mesh_retry.c`
  - Retry tracking for high-priority messages
- `src/led/status_led.c`
  - Status LED initialization and blink helper
- `src/system/factory_reset.c`
  - BOOT-hold factory reset handling
- `src/app/app_init.c`
  - NVS initialization

## Important implementation notes

- `duplicate_node_id_warning` and `littlefs_mounted` intentionally stay in `main.c` and are passed by pointer into HTTP contexts.
- `highest_seen_id` is restored from NVS at boot and saved whenever it changes.
- `packet_counter` is restored from NVS at boot and saved whenever a new local packet ID is assigned.
- Time sync is handled by `mesh_control_handle_time_sync_packet()`, not by duplicate parsing in `main.c`.
- Control-packet classification is shared through `mesh_control_is_control_packet_type()`.

## Verified build state

- `pio run` succeeds.
- Current build warnings are expected to be zero.

## Still open, by design or by intent

- Phase 2: WiFi hardening
- Phase 4: Buffer eviction + dedup scaling
- Phase 8: Smart suppression flooding
- Portal UI P0 work in `data/*.html`

## Notes on tests

- `test/test_core/test_main.c` exists and uses Unity.
- Some test coverage is still duplicated from production logic, so it is useful but not a complete substitute for integration tests.

## Historical files removed

The old split docs were consolidated into this file:

- `docs/FEATURES.md`
- `docs/DECISIONS.md`
- `docs/ROADMAP.md`
- `docs/CHANGELOG.md`
- `BUG_REPORT.md`





shit worth to implement:

18. What I would copy from the reference

This is the important answer given what you've been doing with your refactor.

HIGH VALUE

Neighbor discovery / HELLO concept	YES
Neighbor freshness	YES
RSSI-based next-hop scoring	YES
Exclude previous hop	YES
Top-N route candidates	YES
Random selection among good candidates	YES
Explicit transmission queue	YES
ACK pending state machine	YES
ACK replay protection	YES
Alternate route / ALT concept	YES
Limit alternate attempts	YES
Central message scheduler	YES
TTL-based forwarding	YES