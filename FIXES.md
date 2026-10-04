# ProjectLoRa — Fixes to Prioritize

This is a code-review backlog ranked by impact and likelihood. “Open” means the issue is supported by the current source; “Review” means it needs a design decision or broader audit before implementation. Items marked fixed were rechecked against the current code.

## Priority 0 — Reliability and message delivery

1. **Fix message-store lock ordering and serialize read/modify/write operations** — **Open**
   Location: `src/messages/message_store.c` (`write_record`, `message_store_update_status`, `message_store_remove`, `message_store_delete`).
   `message_store_update_status` holds the store mutex and calls `write_record`, which after its NVS commit tries to take the same non-recursive mutex again. That can deadlock on a successful update. Separately, `write_record` commits NVS before locking and copying its snapshot, so a concurrent writer can commit a newer value and then have the in-memory snapshot overwritten by the older one. `message_store_remove` also looks up a slot, releases the lock, then deletes by slot; another operation can change that slot in between. Use one lock strategy that covers slot selection and persistence consistently, without recursively taking the mutex, and propagate write errors.

2. **Make TX scheduler entries safe across unlocked radio submissions** — **Open**
   Location: `src/mesh/tx_scheduler.c`, especially `send_current_packet` and `scheduler_task`.
   `send_current_packet` drops the scheduler mutex while retaining an `entry` pointer. During that gap, an ACK or another task can deactivate/reuse the slot; the submitter can then update a different message using the stale pointer. Queue submission results also race with radio-result handling. Snapshot the request under lock, submit it unlocked, then reacquire and validate a stable generation/request token before changing the entry.

3. **Correct TX retry and hop-budget accounting** — **Open**
   Location: `src/mesh/tx_scheduler.c` (`prepare_current_packet`, `handle_radio_result`, retry logic).
   `build_forward_packet_v2` consumes a hop, and packet preparation runs again on retries, so retransmitting the same origin message can exhaust its hop budget. Also, `attempts` increments both on radio submission and ACK timeout, making the retry limit/backoff count inconsistent. Preserve the intended hop budget across retries and define/count one attempt per actual transmission.

4. **Persist and restore pending outbound transmissions** — **Open**
   Locations: `src/mesh/tx_scheduler.c`, `src/messages/message_store.c`, `src/app/app_runtime.c`.
   The scheduler is in-memory only and starts empty after reboot. Outbound messages can remain stored as `PENDING`/`SENT` without being re-enqueued, so an outage/restart can strand messages. On boot, identify locally originated messages that are not terminal and restore them to the scheduler, preserving retry and hop semantics.

## Priority 1 — Security and radio correctness

5. **Remove shipped setup credentials and secure provisioning defaults** — **Open**
   Locations: `src/node_config.c`, `data/setup.html`.
   The source and setup page prefill `web_pin=123456789` and `network_key=CHANGEME1234567`; the AP password field also has a sample value. Empty stored values fall back to the same PIN and network key. Require fresh credentials during setup (or generate unique secrets), remove credential `value` defaults, and make unprovisioned state fail closed.

6. **Parse the session cookie as an exact cookie field and use robust session tokens** — **Open**
   Location: `src/http/http_auth.c`.
   `strstr(cookie, expected)` accepts a token embedded in a different cookie value or a duplicate cookie name. The single global session token also means logging in from one client replaces every other client’s session. Parse cookie boundaries exactly, compare the complete value, and decide whether multi-client sessions are required. Add `Secure` when deployment uses HTTPS; note the current portal is HTTP-only.

7. **Route every LoRa transmission through the single TX arbiter** — **Open**
   Locations: `src/mesh/forward_worker.c`, `src/mesh_control.c`, `src/radio/lora_radio.c`.
   `lora_transmit_bytes` enqueues work, but its legacy `lora_radio_submit` path assigns no unique `request_id` or `message_id`. The TX worker still serializes physical transmission, but result events for those requests cannot be reliably correlated to callers. Move forwarding and control packets to the request/result queue API (or explicitly make them fire-and-forget with no result requested), and centralize priority/backpressure policy.

8. **Handle radio/network startup errors without aborting the whole node** — **Open**
   Locations: `src/radio/lora_radio.c`, `src/network/wifi_ap.c`, `src/http/http_server.c`, `src/main.c`.
   Several initialization paths use `ESP_ERROR_CHECK`, which aborts/restarts on peripheral or service failures. Return errors to startup orchestration and keep independent functions available in degraded mode where safe; retain a clear fatal policy for required NVS or security initialization.

## Priority 2 — Product behavior and maintainability

9. **Implement node-role policy or remove the role choice** — **Review**
   Locations: `include/node_config.h`, `src/app/app_runtime.c`, `data/setup.html`.
   `node_role` is configured and displayed but does not change send, receive, or relay permissions. Define concrete role capabilities and enforce them, or remove the selector to avoid misleading operators.

10. **Define duress-PIN behavior or remove the unused setting** — **Review**
    Locations: `include/node_config.h`, `src/node_config.c`, `src/http/http_setup.c`, `data/setup.html`.
    A duress PIN is stored and generated when absent, but authentication never checks it. Specify the intended covert alert and threat model before implementing it; otherwise remove its UI and stored field.

11. **Split `app_runtime.c` by responsibility** — **Review**
    Location: `src/app/app_runtime.c`.
    Runtime startup, packet reception, persistence, forwarding decisions, TX creation, and HTTP wiring are combined in one module. Extract focused components after delivery/security defects are addressed.

12. **Centralize protocol and capacity limits** — **Review**
    Locations: `include/mesh_protocol.h`, `src/messages/message_store.h`, `include/roster.h`.
    Shared limits such as `PAYLOAD_LEN` and `PACKET_LEN` are duplicated, while other capacities live in separate headers. Create a common limits header and remove duplicate definitions.

13. **Add health supervision and structured operational counters** — **Review**
    Locations: `src/radio/lora_radio.c`, task modules under `src/mesh/`, `src/app/app_runtime.c`.
    A radio health-check function exists but is not scheduled; there is no coordinated task liveness/fault reporting or useful counters for queue drops, retries, and recovery. Add supervision and expose actionable diagnostics.

## Rechecked items

- **TTL reset on TX retry — Partially fixed.** `tx_entry_t.remaining_hops` persists the budget between scheduler runs and is copied into the stored message on status updates. However, retries currently consume additional hops during packet construction; see priority 3.
- **Message-store TOCTOU — Partially fixed.** `message_store_update_status` keeps the lock across its read/modify/write, but its nested `write_record` lock can deadlock. `message_store_remove` retains the lookup/delete race; see priority 1.
- **Radio not exclusively serialized — Mostly fixed.** Current direct callers use `lora_transmit_bytes`, which enqueues onto the radio TX task. The legacy wrapper’s result-correlation behavior still needs fixing; see priority 7.
- **TX scheduler state machine is decorative — Outdated.** It processes radio-result events, waits for ACKs, retries with backoff, and completes on ACK. State transitions and retry accounting still have correctness gaps listed above.
- **`duress_pin` declared but unused — Partially accurate.** It is persisted/generated and exposed in setup, but does not participate in authentication or signaling; see priority 10.

## Progress log

- Reviewed the current scheduler, message store, HTTP authentication, configuration defaults, radio submission paths, startup code, and existing audit entries.
- No implementation fixes were made as part of this review; this file records the verified backlog and corrected status of earlier claims.
