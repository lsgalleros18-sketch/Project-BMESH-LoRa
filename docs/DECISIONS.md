# Decisions

- Phase 0: shared `FIELD_LEN`/`SITIO_LEN`/`BARANGAY_LEN`/`MUNICIPALITY_LEN` and `MIN`/`MAX` live in `include/bems_common.h` so `main.c`, `bems_crypto.c`, `mesh_protocol.h`, and `node_config.h` all consume one definition.
- Phase 0: `node_config` state is being moved behind `src/node_config.c` accessors, with the mutex owned by that module instead of `main.c`.
- Phase 0: mesh duplicate tracking state is being moved behind `src/mesh_protocol.c`, with its own private mutex and packet-seen cache.
- Phase 0: LoRa register/SX1278 state is being moved behind `src/lora_radio.c`, with the SPI handle kept private to the module.
- Phase 0: `derive_crypto_keys()` now reads `network_key` from the real `node_config_t` layout through `node_config_get()`, eliminating the offset-0 drift assumption.
- Phase 1: web PIN and network key are generated with `esp_random()` in `src/node_config.c` and persisted immediately; provisioning gate is enforced in `src/bems_crypto.c::derive_crypto_keys()` and `src/lora_radio.c::lora_transmit()`, so unprovisioned nodes cannot encrypt, decrypt, or transmit mesh packets.
- Phase 0: cleanup is now fully closed out with `main.c` calling the module implementations directly, and `pio run -e esp32-s3-devkitm-1` passes after removing the last duplicate config/mesh/radio definitions.
- Phase 1b: setup-time Node ID collision probe uses a 2500 ms timeout so it comfortably covers the mandated jittered reply window (`200 + esp_random() % 1001` ms) with one retry opportunity while still keeping setup responsive.
- Phase 2: the captive AP now uses a distinct WPA2 passphrase stored in `node_config.ap_password` and generated on first boot with `esp_random()`, separate from the portal PIN.
- Phase 2: portal login uses per-client lockout state kept in memory for the session lifetime; failures 1-2 are allowed, failure 3 starts a 30 second lock, and each further failure doubles the lock window.
- Phase 2: successful login now issues a fresh session token per login, and the token expires after 15 minutes of inactivity (`SESSION_IDLE_TIMEOUT_MS = 900000`) so a stale cookie cannot remain valid forever.
- Phase 3: sender-side delivery policy is split by destination/priority. `HIGH` unicast gets ACK tracking with 4 total attempts and a 3 second base retry interval, `NORMAL` unicast gets ACK tracking with 2 total attempts and an 8 second base retry interval, `LOW` unicast is best effort with no ACK tracking, and `ALL` broadcasts are repeated 3 times with jittered spacing instead of waiting for ACKs.
- Phase 3: the UI now surfaces message truth directly: ACK-tracked unicast is shown as `SENT` until `ACKED` or `FAILED`, while broadcasts and no-ACK unicast are shown as `SENT (no delivery confirmation expected)`.
- Phase 4: message-table eviction now prefers the oldest completed entry (`RX`, `ACKED`, or `FAILED`) and returns `NULL` when the table is full of active entries; the portal surfaces that as a queue-full warning instead of silently overwriting in-flight messages.
- Phase 4: dedup lookup uses a 32-bucket hash table for 64 cached packets with a 60 second TTL, keeping the average bucket depth near 2 entries at capacity while preserving TTL pruning and oldest-in-bucket replacement when a bucket must be reused.
- Phase 4: queue-full state is exposed through `/api/status` as `queue_full` and mirrored in the portal warning banner so operators can see why a message was not queued.
- Infra: `sdkconfig.esp32-s3-devkitm-1` now uses a custom `partitions.csv` with a single 3MB factory app, 4MB SPIFFS storage, and 64KB coredump area instead of the built-in single-app 1MB layout; OTA was intentionally not added because this project has no delivery path for firmware updates yet.
