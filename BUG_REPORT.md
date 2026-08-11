# Bug Reports

## Duplicate LoRa Radio Implementations

Phase 5 has now extracted the LoRa radio helpers out of `src/main.c` and into `src/radio/lora_radio.c` with the shared declarations in `src/radio/lora_radio.h`. The duplicate macro block no longer lives in `main.c`, and `lora_rx_task()` now calls the shared raw-frame RX helper instead of inlining the register/FIFO read sequence.

The remaining receive-processing logic in `main.c` is still authoritative for decrypt, parse, dedup, sync, storage, ACK, and forward behavior. The extracted module currently owns the low-level SPI/register helpers plus the new raw-frame RX/TX wrapper functions.
### Current State

The old dead duplicate situation is no longer present in the same form:
- `main.c` no longer carries the duplicate LoRa register/mode/macro definitions
- `src/radio/lora_radio.c` now contains the shared radio primitives
- `src/radio/lora_radio.h` now exposes the shared constants and helper declarations

Build verification remains `UNVERIFIED` in this shell because the ESP-IDF toolchain is not installed here (`idf.py` is unavailable on PATH).

## Phase 6-8 Merge Discrepancy

The packet helper merge from `main.c` into `src/mesh_protocol.c` is not byte-for-byte identical.

Confirmed difference:
- `build_forward_packet()` in the old `main.c` version formatted `parsed->payload` with a width of `48`, while `src/mesh_protocol.c` briefly used `120` after the Phase 6-8 merge.

No other difference was found in the compared bodies for:
- `packet_seen()`
- `remember_packet()`
- `parse_mesh_packet()`
- the rest of `build_forward_packet()` aside from the payload width noted above

This discrepancy was discovered from git history, was introduced by the Phase 6-8 merge, and has now been corrected back to the original width of `48`.
