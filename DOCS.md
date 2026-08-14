# Project Docs

This is the single source of truth for the ProjectLoRa codebase docs.

## What the code currently does

- ESP32-S3 ESP-IDF project with a PlatformIO build path.
- Captive portal served from LittleFS files in `data/`:
  - `data/index.html`
  - `data/setup.html`
  - `data/login.html`
- LoRa mesh messaging with encrypted BEMS frames.
- Binary V2 transmit for normal messages, forwarding, retries, ACK, SYNC_REQ, SYNC_RESP, and TIME_SYNC.
- V1 receive compatibility remains in the parser path only.
- TX/RX, broadcast delivery, jittered forwarding, dedup suppression, ACK handling.
- NVS-backed node configuration and factory reset flow.
- NVS-backed message store with bounded retention.
- NVS-backed `highest_seen_id` persistence in `mesh_control`.
- NVS-backed packet counter persistence in `main.c`.
- Time sync still parses peer `epoch=` and `~dist=` payload data on receive.

## V2 wire format

- `mesh_protocol.c` owns the binary V2 serializer/parser.
- V2 packets are length-driven.
- The wire path uses explicit field lengths for string data.
- Reserved broadcast destination `ALL` is preserved as a special destination value.
- `build_forward_packet_v2()` is the canonical V2 serializer.
- `parse_mesh_packet_v2()` is the canonical V2 parser.
- The established maximum plaintext limit remains `227` bytes.

## SYNC_RESP

- `SYNC_RESP` now uses a strict binary record payload.
- The payload starts with a record count byte.
- Each record contains:
  - `id` as little-endian `uint32_t`
  - source length + raw source bytes
  - destination length + raw destination bytes
  - type length + raw type bytes
  - priority length + raw priority bytes
  - hops as a single byte
  - payload length + raw payload bytes
- The decoder is length-driven and rejects truncated or malformed records.

## V1 compatibility

- V1 is still accepted on receive.
- V1 compatibility is isolated to the receive path.
- There is no V1 transmit mode in the production path.

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
  - ACK generation
  - SYNC_RESP binary serialization/decoding
- `src/mesh_protocol.c`
  - V1 parser
  - V2 binary parser/serializer
  - Deduplication storage
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
- Production transmit paths use `lora_transmit_bytes()`.

## Verified build state

- `pio run -e esp32-s3-devkitm-1` succeeds.
- Current build warnings are expected to be zero.

## Notes on tests

- `test/test_core/test_main.c` exists and uses Unity.
- Unit coverage includes V2 parsing/serialization, SYNC_RESP decoding, routing, replay, dedup, DNS parsing, and message store behavior.
- `pio test` was attempted, but hardware upload failed in this environment with `*** [upload] Error 2`.
- Compilation of the test target succeeded before the upload step failed.

## Historical files removed

The old split docs were consolidated into this file:

- `docs/FEATURES.md`
- `docs/DECISIONS.md`
- `docs/ROADMAP.md`
- `docs/CHANGELOG.md`
- `BUG_REPORT.md`
