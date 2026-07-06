# Decisions

- Phase 0: shared `FIELD_LEN`/`SITIO_LEN`/`BARANGAY_LEN`/`MUNICIPALITY_LEN` and `MIN`/`MAX` live in `include/bems_common.h` so `main.c`, `bems_crypto.c`, `mesh_protocol.h`, and `node_config.h` all consume one definition.
- Phase 0: `node_config` state is being moved behind `src/node_config.c` accessors, with the mutex owned by that module instead of `main.c`.
- Phase 0: mesh duplicate tracking state is being moved behind `src/mesh_protocol.c`, with its own private mutex and packet-seen cache.
- Phase 0: LoRa register/SX1278 state is being moved behind `src/lora_radio.c`, with the SPI handle kept private to the module.
- Phase 0: `derive_crypto_keys()` now reads `network_key` from the real `node_config_t` layout through `node_config_get()`, eliminating the offset-0 drift assumption.
