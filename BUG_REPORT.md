# Bug Reports

## Duplicate LoRa Radio Implementations

The active firmware path is implemented as `static` radio functions in `src/main.c`, including its own SPI state, semaphores, ISR, RX task, initialization, transmit path, and receive path. The repository also contains a separate `src/lora_radio.c` implementation with overlapping register definitions, hardware state, initialization, transmit APIs, and receive APIs declared in `include/lora_radio.h`.

These implementations are not behaviorally identical. The `main.c` path owns the active packet-processing task and invokes its own static functions, while the standalone module exposes a different receive/decrypt path and additional provisioning behavior. Selecting or merging either implementation during Phase 5 without runtime evidence would change behavior. Phase 5 radio extraction is therefore blocked until the authoritative implementation is identified.
### Authoritative and Dead-Code Confirmation

Call-site tracing confirms that `src/main.c` is authoritative: `app_main()` invokes its `lora_init()` path, and the active transmit and `lora_rx_task()` paths are all called from `main.c`. The `src/lora_radio.c` functions declared in `include/lora_radio.h` have zero callers in the tree. That module is therefore confirmed dead code for the current firmware path, but remains unchanged pending a separate reviewable removal step.
