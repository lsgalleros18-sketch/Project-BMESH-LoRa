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














I audited the uploaded `Pr444ojectLoRa.zip` as the current baseline. The project is much better than the earlier versions: the V2 binary protocol, replay protection, deduplication, routing table, TX scheduler, storage layer, HTTP modules, and test suite are already there.

However, I would **not** call it production-ready yet. There are several architectural bugs that can cause duplicate transmissions, forwarding loops, radio corruption, stale messages after reboot, resource exhaustion, and authentication weaknesses.

Also, I could not perform a real ESP-IDF/PlatformIO build in this environment because `pio`/`idf.py` are not installed here, so this is a **source-level audit plus protocol/concurrency review**, not a hardware/build certification.

# (DONE)1. Fix these first — highest priority

## 1.1 Stop transmitting every new message twice

This is the most obvious logic bug I found.

In `app_runtime.c`, `queue_message()` does:

```c
(void)lora_transmit_bytes(packet_buf, packet_len);
(void)tx_scheduler_enqueue(&message, nvs_slot);
```

But `tx_scheduler_enqueue()` puts the message into the scheduler, and `scheduler_task()` later calls `lora_transmit_bytes()` again.

So the flow is effectively:

```text
HTTP /send
   ↓
build packet
   ↓
transmit immediately
   ↓
enqueue scheduler
   ↓
scheduler sees QUEUED
   ↓
transmit again
```

### Correct architecture

`queue_message()` should **never transmit directly**.

Make the flow:

```text
HTTP request
   ↓
validate request
   ↓
create message
   ↓
store message as QUEUED
   ↓
enqueue TX job
   ↓
TX worker sends once
   ↓
WAIT_ACK
   ↓
ACK → DELIVERED
   ↓
timeout → retry
   ↓
max retries → FAILED
```

Change:

```c
(void)lora_transmit_bytes(packet_buf, packet_len);
(void)tx_scheduler_enqueue(&message, nvs_slot);
```

to effectively:

```c
if (!tx_scheduler_enqueue(&message, nvs_slot)) {
    // mark message FAILED / QUEUE_FULL
}
```

Better yet, let the scheduler own packet construction too.

### Add

```c
bool tx_scheduler_submit(
    const emergency_message_t *message,
    int nvs_slot
);
```

and make the scheduler responsible for:

```text
message
→ choose route
→ construct packet
→ radio queue
→ transmit
→ wait for result
```

---

# (DONE)2. Fix hop/TTL handling

This is another major networking bug.

You use:

```c
if (packet->hops <= 0) {
    return false;
}
```

but when forwarding, the packet's hop count is never actually decremented.

In `delayed_forward_task()` you copy the packet and transmit it, but there is no:

```c
packet.hops--;
```

Therefore:

```text
NODE A
  ↓ hops=5
NODE B
  ↓ hops=5
NODE C
  ↓ hops=5
NODE D
  ↓ hops=5
...
```

The hop count is not actually a hop limit.

Deduplication prevents the same node from forwarding the exact same packet repeatedly, but the packet can still circulate around a network until every node has seen it.

## Correct logic

Every forwarding operation must do:

```c
if (packet.hops == 0) {
    drop;
    return;
}

packet.hops--;

if (packet.hops == 0 && destination != local_node) {
    do not forward;
}
```

I recommend making this an explicit function:

```c
bool mesh_packet_consume_hop(
    mesh_packet_t *packet
);
```

Implementation:

```c
bool mesh_packet_consume_hop(mesh_packet_t *packet)
{
    if (packet == NULL) {
        return false;
    }

    if (packet->hops <= 0) {
        return false;
    }

    packet->hops--;
    return packet->hops > 0;
}
```

Then forwarding becomes:

```c
if (!mesh_packet_consume_hop(&packet)) {
    return;
}
```

---

# (DONE)3. Separate TTL from routing distance

Right now `hops` is doing too many jobs.

You are using it as:

```text
TTL / forwarding budget
```

but also:

```text
route metric
```

For example:

```c
route_table_learn(
    parsed->source,
    parsed->relay,
    parsed->hops,
    rssi
);
```

That is not a reliable routing metric.

A packet's remaining TTL does not tell you its actual path length.

## Better design

Eventually use:

```c
uint8_t ttl;
uint8_t path_cost;
```

conceptually:

```text
TTL = how many more nodes can forward
PATH = how many hops from origin
```

I would **not immediately break your V2 wire format** for this. First stabilize V2.

For V2:

```text
hops = TTL
```

and use routing decisions based primarily on:

```text
RSSI
link freshness
next-hop reliability
known neighbor
```

Then introduce V3 later if you need explicit route metrics.

---

# (DONE)4. Serialize all LoRa radio access

This is probably the biggest concurrency problem in the firmware.

You have multiple independent callers:

```text
TX scheduler
delayed_forward_task()
mesh_control
HTTP-triggered sends
time sync
ACK sends
SYNC response
```

all eventually call:

```c
lora_transmit_bytes()
```

But the radio driver does not have a global TX mutex.

The variable:

```c
static volatile bool radio_in_tx;
```

is **not sufficient synchronization**.

Imagine:

```text
Task A:
  radio_in_tx = true
  start TX

Task B:
  radio_in_tx = true
  changes FIFO

Task A:
  waits for TX_DONE

DIO0 ISR:
  which transaction does this belong to?
```

Now the FIFO, operation mode and semaphore state can become inconsistent.

## Fix

The radio driver should be the **only component allowed to touch the SX1278**.

Create:

```text
src/radio/
    lora_radio.c
    lora_radio.h
    lora_tx_queue.c
    lora_tx_queue.h
    lora_radio_state.c
```

and expose:

```c
bool lora_radio_submit(
    const uint8_t *packet,
    size_t length,
    lora_tx_priority_t priority
);
```

Then create one TX worker:

```text
            ┌────────────┐
HTTP ──────►│            │
Mesh ──────►│ TX queue   │
ACK ───────►│            │
SYNC ──────►│            │
Forward ───►│            │
            └─────┬──────┘
                  ↓
             lora_tx_task
                  ↓
               SX1278
```

Only `lora_tx_task` calls:

```c
lora_write_fifo()
lora_set_mode()
lora_receive_mode()
```

This gives you a single radio owner.

---

# (DONE)5. Fix the SX1278 CAD/channel-clear implementation

There is a concrete register problem in the current radio code.

You define:

```c
#define REG_IRQ_FLAGS_1 0x3E
```

and use it as:

```c
uint8_t irq_flags_1 = lora_read_reg(REG_IRQ_FLAGS_1);
```

for CAD status.

For SX1276/77/78/79 LoRa mode, the CAD status bits are in `RegIrqFlags` at `0x12`, including `CadDone` and `CadDetected`. The datasheet identifies `0x12` as the LoRa IRQ register with those bits. ([SparkFun][1])

So your CAD implementation should use:

```c
REG_IRQ_FLAGS
```

and inspect:

```c
IRQ_CAD_DONE
IRQ_CAD_DETECTED
```

instead of treating `0x3E` as a second IRQ register.

This needs to be corrected before trusting your channel-busy logic.

---

# (DONE)6. Make the radio driver error-aware

Currently many functions ignore SPI errors.

For example:

```c
static uint8_t lora_read_reg(...)
{
    uint8_t value = 0;
    lora_transfer(...);
    return value;
}
```

If SPI fails, the caller receives:

```text
0
```

and may interpret that as an actual register value.

## Change the API

Instead of:

```c
uint8_t lora_read_reg(...)
```

use:

```c
esp_err_t lora_read_reg(
    uint8_t address,
    uint8_t *value
);
```

Then:

```c
uint8_t version;

if (lora_read_reg(REG_VERSION, &version) != ESP_OK) {
    lora_radio_enter_fault();
    return;
}
```

Do the same for:

```c
lora_write_reg()
lora_transfer()
lora_read_fifo()
lora_write_fifo()
```

---

# (DONE)7. Add a proper radio state machine

Instead of only:

```c
bool lora_ready;
bool radio_in_tx;
```

use:

```c
typedef enum {
    RADIO_STATE_UNINITIALIZED,
    RADIO_STATE_INITIALIZING,
    RADIO_STATE_RX,
    RADIO_STATE_TX,
    RADIO_STATE_CAD,
    RADIO_STATE_FAULT,
    RADIO_STATE_RECOVERING
} radio_state_t;
```

Functions:

```c
radio_state_t lora_radio_get_state(void);

bool lora_radio_is_ready(void);

bool lora_radio_recover(void);

esp_err_t lora_radio_reset(void);

bool lora_radio_health_check(void);
```

### Recovery sequence

If:

```text
TX timeout
SPI error
invalid IRQ state
SX1278 disappears
```

do:

```text
stop TX
↓
clear IRQ
↓
standby
↓
reset SX1278
↓
reinitialize registers
↓
verify version
↓
return RX mode
```

Do not simply continue with:

```c
lora_ready = true;
```

after a failed radio operation.

---

# (DONE)8. Fix message persistence architecture

This part needs substantial work.

`message_store_remove()` removes the message from RAM:

```c
compact_remove_index(index);
```

but it does **not remove the corresponding NVS record**.

That means:

```text
RAM:
message deleted

reboot

NVS:
old message still exists

boot:
old message comes back
```

That is a real persistence bug.

## Bigger problem

Your NVS slot is based on array index:

```c
slot_index = (int)(slot - messages);
```

but your RAM store can compact messages.

So:

```text
NVS slot 0 → message A
NVS slot 1 → message B
NVS slot 2 → message C

remove B

RAM:
slot 0 A
slot 1 C
```

Now the mapping no longer represents the original persistent record.

---

# (DONE)9. Replace the message store with fixed persistent slots

Do **not compact persistent slots**.

Use:

```text
message_slot[0]
message_slot[1]
...
message_slot[15]
```

Each slot has:

```c
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t state;
    uint32_t checksum;
    emergency_message_t message;
} persisted_message_t;
```

States:

```c
MESSAGE_SLOT_EMPTY
MESSAGE_SLOT_QUEUED
MESSAGE_SLOT_SENT
MESSAGE_SLOT_ACKED
MESSAGE_SLOT_FAILED
```

Deleting:

```c
state = MESSAGE_SLOT_EMPTY;
```

not memory compaction.

Functions:

```c
esp_err_t message_store_init(void);

bool message_store_allocate(
    int *slot
);

esp_err_t message_store_write(
    int slot,
    const emergency_message_t *message
);

esp_err_t message_store_update(
    int slot,
    const emergency_message_t *message
);

esp_err_t message_store_delete(
    int slot
);

bool message_store_read(
    int slot,
    emergency_message_t *message
);
```

This makes reboot behavior deterministic.

---

# (DONE)10. Add message-store integrity checking

Never assume an NVS blob is valid.

Add:

```c
magic
version
length
CRC32
state
```

Example:

```c
#define MESSAGE_STORE_MAGIC 0x424D5347u
#define MESSAGE_STORE_VERSION 1
```

Validation:

```c
if (record.magic != MESSAGE_STORE_MAGIC)
    reject;

if (record.version != MESSAGE_STORE_VERSION)
    reject;

if (crc32(record.payload) != record.crc)
    reject;
```

This protects against:

```text
power loss
partial writes
firmware upgrade
corrupt NVS
unexpected old structures
```

---

# (DONE)11. Stop doing flash/NVS writes while holding global application locks

`store_received_packet()` currently does:

```c
data_lock();
...
storage_message_save();
...
data_unlock();
```

and also calls:

```c
roster_touch()
route_table_learn()
```

while holding the application's `data_mutex`.

That is too broad.

You want independent locking domains:

```text
config_mutex
message_mutex
route_mutex
roster_mutex
tx_mutex
radio_tx_queue
```

Never:

```text
global data mutex
    ↓
NVS
    ↓
roster
    ↓
route
```

## Correct pattern

Build a local copy first:

```c
emergency_message_t message;
```

Then:

```c
message_store_add(...)
```

Then independently:

```c
storage_message_save(...)
```

Then:

```c
roster_touch(...)
route_table_learn(...)
```

No giant cross-module lock.

---

# (DONE)12. Fix `data_lock()` architecture

You currently have:

```c
static SemaphoreHandle_t data_mutex;
```

inside `app_runtime`.

This makes `app_runtime.c` the implicit owner of shared application state.

That is exactly what the refactor was supposed to eliminate.

Remove:

```c
data_lock()
data_unlock()
```

from the central runtime.

Each module should own its data and its mutex.

The runtime should coordinate modules, not provide a universal lock.

---

# (DONE)13. Replace one-task-per-forward with a bounded queue

Currently every incoming packet can do:

```c
pvPortMalloc(...)
xTaskCreate(delayed_forward_task, ...)
```

That means a packet burst can create many tasks.

For example:

```text
100 packets
→ 100 heap allocations
→ 100 FreeRTOS tasks
```

That is not a robust embedded architecture.

## Replace with

```text
src/mesh/forward_queue.c
src/mesh/forward_worker.c
```

Queue:

```c
QueueHandle_t forward_queue;
```

Entry:

```c
typedef struct {
    mesh_packet_t packet;
    int rssi;
    int snr;
    route_entry_t route;
    bool route_known;
} forward_job_t;
```

Then:

```text
RX
 ↓
validate
 ↓
dedup
 ↓
xQueueSend(forward_queue)
 ↓
forward_worker
 ↓
jitter
 ↓
re-check dedup/cancellation
 ↓
forward
```

Use a fixed queue:

```c
#define FORWARD_QUEUE_DEPTH 8
```

or 16.

Now memory consumption is deterministic.

---

# (DONE)14. Add forwarding priority

Emergency traffic should not wait behind routine traffic.

Use:

```c
typedef enum {
    TX_PRIORITY_HIGH,
    TX_PRIORITY_NORMAL,
    TX_PRIORITY_LOW
} tx_priority_t;
```

Separate queues:

```text
HIGH
NORMAL
LOW
```

Scheduler policy:

```text
2 HIGH
→ 1 NORMAL
→ 1 LOW
```

or strict priority for emergencies.

This is much more useful for a disaster/emergency mesh than treating all packets equally.

---

# (DONE)15. Make the TX scheduler a real state machine

Your existing scheduler is a good start, but it should be strengthened.

Current:

```text
QUEUED
SENDING
WAITING_ACK
RETRY_PENDING
DELIVERED
FAILED
```

Add:

```text
CREATED
QUEUED
WAITING_RADIO
TRANSMITTING
WAITING_ACK
RETRY_BACKOFF
DELIVERED
EXPIRED
FAILED_RADIO
FAILED_QUEUE
FAILED_TIMEOUT
```

Store the reason:

```c
typedef enum {
    TX_ERROR_NONE,
    TX_ERROR_RADIO_BUSY,
    TX_ERROR_RADIO_TIMEOUT,
    TX_ERROR_QUEUE_FULL,
    TX_ERROR_PACKET_BUILD,
    TX_ERROR_ACK_TIMEOUT,
    TX_ERROR_INVALID_ROUTE
} tx_error_t;
```

That lets the UI show:

```text
FAILED
Reason: ACK timeout
```

instead of simply:

```text
FAILED
```

---

# (DONE)16. Do not hold `tx_mutex` during radio transmission

Right now scheduler processing holds the scheduler lock while it eventually calls:

```c
lora_transmit_bytes()
```

which may block for up to 5 seconds.

That means the TX state lock becomes a radio-operation lock.

Bad.

Change to:

```text
lock
↓
select job
↓
mark TRANSMITTING
↓
unlock
↓
radio transmit
↓
lock
↓
update state
↓
unlock
```

This allows ACK handling to occur independently.

---

# (DONE)17. Add duplicate ACK protection explicitly

`tx_scheduler_acknowledge()` already has some protection, but make it a formal rule:

```text
WAITING_ACK
   ↓
ACK
   ↓
DELIVERED
   ↓
any later ACK
   ↓
ignore
```

Also verify:

```text
ack.source == original destination
ack.id == original message ID
ack.destination == local node
```

And avoid accepting arbitrary payload strings such as:

```text
ACK for 123
```

as the only authentication structure.

The ACK should ideally carry:

```text
original message ID
original source
destination
ACK type
```

inside the authenticated binary protocol.

---

# (DONE)18. Stop using string parsing for ACK IDs

Current:

```c
"ACK for 123"
```

then:

```c
strtoul(...)
```

Make ACK a structured V2 control packet.

Conceptually:

```c
typedef struct {
    uint32_t acknowledged_id;
    char acknowledged_source[FIELD_LEN];
} ack_info_t;
```

Then:

```c
mesh_control_build_ack(...)
mesh_control_parse_ack(...)
```

This removes fragile string parsing.

---

# 19. Harden V1 parsing

You correctly isolated V1 to receive compatibility, which is good.

Keep that.

But V1 should have a strict validation function:

```c
bool validate_v1_packet(const mesh_packet_t *packet);
```

Check:

```text
ID != 0
source valid
destination valid
type known
priority known
hops 0..255
payload <= PAYLOAD_LEN
relay valid
no malformed separators
```

Do not merely parse and trust fields.

---

# 20. Harden V2 validation

Your V2 parser is much better, but add a dedicated validation layer.

Current:

```text
decrypt
→ parse
→ use packet
```

Make:

```text
decrypt
→ structural parse
→ semantic validation
→ replay check
→ dedup
→ route/delivery
```

Add:

```c
bool mesh_packet_validate(const mesh_packet_t *packet);
```

Validation includes:

```text
source != empty
source != oversized
destination != empty
destination allowed
relay != empty
type known
priority known
hops <= MAX_HOPS
payload length correct
broadcast rules valid
next-hop rules valid
```

---

# 21. Add maximum-hop constants

Do not scatter numbers like:

```c
5
3
1
```

Create:

```c
#define MESH_MAX_TTL 8
#define MESH_HIGH_TTL 8
#define MESH_NORMAL_TTL 5
#define MESH_LOW_TTL 3
```

Then:

```c
static uint8_t ttl_for_priority(...)
```

Use `uint8_t`, not signed `int`, in the protocol model.

---

# 22. Improve route-table semantics

The route table is currently an opportunistic cache.

That's fine, but make its rules explicit.

Each route should have:

```c
typedef struct {
    char destination[FIELD_LEN];
    char next_hop[FIELD_LEN];

    uint16_t metric;
    uint16_t reliability;

    int16_t last_rssi;
    int16_t last_snr;

    uint32_t last_seen_ms;

    uint16_t tx_attempts;
    uint16_t tx_failures;

    bool stale;
} route_entry_t;
```

Then calculate:

```text
metric =
    hop_penalty
  + RSSI penalty
  + age penalty
  + failure penalty
```

rather than using several competing rules.

---

# 23. Add route failure feedback

Currently route selection does not strongly learn from failed transmissions.

Add:

```c
route_table_report_tx_success(destination, next_hop);

route_table_report_tx_failure(destination, next_hop);

route_table_report_ack_timeout(destination, next_hop);
```

Example:

```text
success:
 reliability ↑

failure:
 failure_count ↑
 metric ↑

repeated failures:
 invalidate route
```

Then select the next route candidate.

---

# 24. Do not let stale routes silently remain preferred

Define:

```c
#define ROUTE_STALE_MS 300000
```

but also use multiple states:

```text
ACTIVE
SUSPECT
STALE
INVALID
```

Example:

```text
0–60 sec      ACTIVE
60–180 sec    SUSPECT
180–300 sec   STALE
>300 sec      INVALID
```

This makes routing much more predictable.

---

# 25. Add broadcast suppression policy

For `ALL`, forwarding should be different from unicast.

Correct:

```text
broadcast received
↓
dedup
↓
deliver locally
↓
if TTL > 0
    delayed broadcast forward
```

Do not route broadcast via route table.

The current structure is already partly doing this, but formalize it in:

```c
mesh_forward_should_forward_broadcast()
mesh_forward_should_forward_unicast()
```

---

# 26. Add random backoff based on RSSI

Your current:

```c
100 + random % 500
```

is okay as a starting point.

Improve it:

```text
strong RSSI
→ shorter backoff

weak RSSI
→ longer backoff
```

And cancel when another node already forwarded the same packet.

Example:

```c
uint32_t forwarding_backoff_ms(
    int rssi,
    uint32_t random_seed
);
```

---

# 27. Fix HTTP request length handling

Both `/login` and `/setup` use fixed buffers but allow a request larger than the buffer to be partially consumed.

That can leave unread body data on the HTTP connection.

Add:

```c
if (request->content_len >= sizeof(body)) {
    httpd_resp_set_status(request, "413 Payload Too Large");
    return httpd_resp_send(...);
}
```

Before reading.

Do this to every POST endpoint:

```text
/login
/setup
/reset
/send
/sync
/settime
```

---

# 28. Fix `form_value()`

The function:

```c
size_t copy_len = MIN(value_len, output_size - 1);
```

assumes:

```c
output_size > 0
```

Add:

```c
if (body == NULL ||
    key == NULL ||
    output == NULL ||
    output_size == 0) {
    return false;
}
```

Also make the parser reject malformed percent encoding rather than silently preserving bad `%`.

---

# 29. Fix session-token matching

Current code:

```c
if (strstr(cookie, expected) == NULL)
```

is not a proper cookie parser.

Parse individual cookie fields:

```text
Cookie:
foo=x; BMESH_SESSION=ABC; bar=y
```

and compare the exact field.

Implement:

```c
bool http_auth_cookie_matches(
    const char *cookie_header,
    const char *token
);
```

Use constant-time comparison for the token.

---

# 30. Make sessions per client

Currently the session state is global:

```c
static char session_token[SESSION_TOKEN_LEN];
static TickType_t session_last_activity_tick;
```

That means the entire portal has effectively one session.

Better:

```c
#define MAX_HTTP_SESSIONS 4

typedef struct {
    bool active;
    char token[SESSION_TOKEN_LEN];
    int socket_fd;
    TickType_t last_activity;
} http_session_t;
```

Then:

```text
login
→ allocate session
→ bind token to connection
```

This also makes timeout handling correct.

ESP-IDF's HTTP server supports session-specific context and configurable client sockets; the application is responsible for synchronizing application-level state as needed. ([GitHub][2])

---

# 31. Use secure cookie attributes

Current:

```text
HttpOnly; SameSite=Lax
```

Add where applicable:

```text
Secure
```

once HTTPS is available.

For the current local HTTP-only deployment, at minimum keep:

```text
HttpOnly
SameSite=Strict
```

for an administrative portal.

---

# 32. Fix the default credentials

This is a major production-security issue.

Current defaults include:

```text
123456789
CHANGEME1234567
```

and the setup page also exposes these defaults.

Do not ship a deployed node using these.

Better:

### On first boot

Generate:

```text
random web PIN
random network key
random AP password
```

Store them in NVS.

Display them only during initial commissioning.

Or force the operator to configure them before enabling the mesh.

---

# 33. Make provisioning a real state machine

Instead of:

```c
configured = true;
```

use:

```c
NODE_STATE_UNPROVISIONED
NODE_STATE_PROVISIONING
NODE_STATE_PROVISIONED
NODE_STATE_DEGRADED
```

Then:

```text
UNPROVISIONED
 → Setup portal

PROVISIONED
 → normal mesh

DEGRADED
 → portal + diagnostic state
```

This prevents partially configured nodes from pretending they are healthy.

---

# 34. Validate node identity properly

`node_config_copy_node_id()` filters characters, but filtering invalid characters is not the same as validation.

Reject:

```text
empty
too long
ALL
BROADCAST
NODE IDs with reserved prefixes
duplicate local ID
```

Add:

```c
bool node_config_validate_id(
    const char *node_id,
    char *error,
    size_t error_size
);
```

---

# 35. Make `node_role` actually control behavior

You have:

```text
relay-only
household
command-post
```

but they are mostly metadata.

Turn it into policy.

For example:

```c
typedef enum {
    NODE_ROLE_RELAY,
    NODE_ROLE_HOUSEHOLD,
    NODE_ROLE_COMMAND_POST
} node_role_t;
```

Policies:

### Relay

```text
forward
receive
routing
minimal UI
```

### Household

```text
send
receive
limited forwarding
```

### Command Post

```text
receive all
store more
dashboard
priority traffic
```

Centralize:

```c
bool node_role_can_forward(...);
bool node_role_can_send(...);
bool node_role_can_admin(...);
```

---

# 36. Either implement `duress_pin` properly or remove it

The configuration contains:

```c
duress_pin
```

but there is no real operational behavior behind it.

Do not leave security-sensitive features half implemented.

Either:

```text
Duress PIN
→ hidden distress state
→ emergency notification
→ altered UI
→ stored event
```

or remove it until fully designed.

---

# 37. Add watchdog supervision

Create:

```text
src/system/watchdog.c
src/system/health_monitor.c
```

Monitor:

```text
radio task
TX scheduler
DNS
HTTP
forward worker
storage
```

Each task periodically reports:

```c
health_monitor_heartbeat(TASK_RADIO);
```

Then:

```text
if radio heartbeat stale
    restart radio

if TX worker stale
    restart TX worker

if system health catastrophic
    controlled reboot
```

---

# 38. Add stack-watermark monitoring

For every important FreeRTOS task:

```c
uxTaskGetStackHighWaterMark(NULL);
```

Record:

```text
radio
HTTP
DNS
TX
forward
time sync
factory reset
```

Expose these in `/api/status`.

Example:

```json
{
  "tasks": {
    "radio": {
      "stack_free": 2216
    },
    "tx": {
      "stack_free": 1872
    }
  }
}
```

This will catch problems before random crashes.

---

# 39. Add heap monitoring

Expose:

```c
heap_caps_get_free_size(...)
heap_caps_get_minimum_free_size(...)
heap_caps_get_largest_free_block(...)
```

Dashboard:

```text
Free heap
Minimum heap
Largest block
PSRAM
```

And warn:

```text
HEAP LOW
```

before the node becomes unstable.

---

# 40. Add a real system health model

Create:

```c
typedef struct {
    bool nvs_ok;
    bool littlefs_ok;
    bool radio_ok;
    bool wifi_ok;
    bool http_ok;
    bool dns_ok;
    bool time_synced;
    uint32_t free_heap;
    uint32_t min_heap;
    uint32_t tx_queue_depth;
    uint32_t forward_queue_depth;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t tx_failures;
    uint32_t rx_auth_failures;
    uint32_t replay_drops;
    uint32_t dedup_drops;
} system_health_t;
```

Then:

```c
system_health_get(system_health_t *out);
```

This becomes the single source for your dashboard.

---

# 41. Add counters everywhere

At minimum:

```text
rx_total
rx_valid
rx_invalid
rx_auth_failed
rx_replay_dropped
rx_duplicate
rx_for_me
rx_broadcast

tx_total
tx_success
tx_failed
tx_retries
tx_ack_timeout

forward_total
forward_success
forward_suppressed
forward_ttl_expired
forward_duplicate

route_hits
route_misses
route_failures
```

These counters make debugging the real network dramatically easier.

---

# 42. Add event logging instead of only `ESP_LOGI`

Create:

```c
mesh_event_log(...)
```

Events:

```text
NODE_BOOT
NODE_CONFIGURED
RADIO_READY
RADIO_FAULT
TX_QUEUED
TX_SENT
TX_ACKED
TX_FAILED
RX_ACCEPTED
RX_REPLAY
RX_DUPLICATE
FORWARD
ROUTE_LEARNED
ROUTE_EXPIRED
STORAGE_ERROR
```

Then retain the latest 100 or 200 events in RAM.

The dashboard can show:

```text
20:31:04 TX 31 → BRGY001 ACKED
20:31:02 RX NODE02 → NODE01
20:30:51 Route NODE04 via NODE02
20:30:49 Radio recovered
```

This is hugely useful during actual field testing.

---

# 43. Strengthen the crypto layer

Your current design is:

```text
AES-CTR
+
HMAC-SHA256
```

and the HMAC authenticates the encrypted frame header and ciphertext.

That is fundamentally much better than unauthenticated encryption.

However, the long-term architecture should move toward a standard AEAD construction such as:

```text
AES-GCM
```

or:

```text
ChaCha20-Poly1305
```

when your ESP-IDF/PSA configuration allows it.

Do **not** silently change the existing V2 crypto framing.

Instead:

```text
V2 crypto
→ remain compatible

V3 crypto
→ AEAD
```

---

# 44. Strengthen nonce uniqueness

Current encryption generates an 8-byte random nonce:

```c
esp_random()
```

That is reasonable, but a deterministic uniqueness guarantee is stronger.

Create:

```c
uint64_t crypto_nonce_counter;
```

stored safely in NVS, or use a construction incorporating:

```text
node identity
+
counter
+
boot randomness
```

Then guarantee that a node never reuses a nonce with the same key.

This matters especially because AES-CTR absolutely requires unique nonce/counter combinations.

---

# 45. Do not expose the network key through normal status APIs

Audit every HTTP response for:

```text
network_key
web_pin
duress_pin
AP password
```

These should never appear in:

```text
/api/status
/api/messages
debug pages
logs
```

Also avoid logging:

```c
ESP_LOGI(... network_key ...)
```

---

# 46. Remove secrets from source-controlled HTML

`setup.html` currently visibly contains defaults such as:

```html
value='123456789'
value='CHANGEME1234567'
```

Do not ship credentials in frontend files.

Use:

```text
placeholder
```

instead.

Generate/show initial credentials dynamically.

---

# 47. Improve time synchronization

Current time synchronization depends heavily on payload values.

Create a formal clock module:

```text
src/system/time_service.c
src/system/time_service.h
```

Functions:

```c
bool time_service_is_synced(void);

uint32_t time_service_now(void);

void time_service_apply_sync(
    uint32_t epoch,
    uint32_t source_id,
    uint8_t distance
);
```

Maintain:

```text
last sync
source
quality
age
offset
```

Then reject obviously bad time updates.

---

# 48. Never allow arbitrary time sources to win

Use:

```text
trusted source
+
shortest distance
+
newer timestamp
+
better RSSI
```

rather than simply:

```text
last packet wins
```

Otherwise a bad/misconfigured node can poison the network clock.

---

# 49. Make boot initialization deterministic

Currently initialization is spread across:

```text
app_init
node_config
radio
storage
scheduler
WiFi
HTTP
DNS
```

Define:

```c
typedef enum {
    BOOT_STAGE_NVS,
    BOOT_STAGE_CONFIG,
    BOOT_STAGE_STORAGE,
    BOOT_STAGE_RADIO,
    BOOT_STAGE_NETWORK,
    BOOT_STAGE_SERVICES,
    BOOT_STAGE_READY
} boot_stage_t;
```

Then:

```text
NVS
 ↓
identity
 ↓
config
 ↓
storage
 ↓
radio
 ↓
mesh services
 ↓
WiFi/AP
 ↓
HTTP
 ↓
DNS
 ↓
TX scheduler
 ↓
READY
```

Do not start dependent tasks before their dependencies are initialized.

---

# 50. Change `app_runtime.c` into a coordinator

`app_runtime.c` is still too large.

It is currently doing:

```text
boot
RX
message storage
forwarding
TX
HTTP wiring
time sync
node identity
```

Break it into:

```text
src/app/app_runtime.c
src/app/app_boot.c

src/mesh/mesh_rx.c
src/mesh/mesh_forward.c
src/mesh/mesh_tx.c
src/mesh/mesh_dispatch.c
src/mesh/mesh_metrics.c

src/system/health_monitor.c
```

The desired `app_runtime.c` should look approximately like:

```c
void app_runtime_start(void)
{
    app_boot_init();

    mesh_rx_start();
    mesh_forward_start();
    mesh_tx_start();

    network_services_start();
    system_services_start();

    health_monitor_start();
}
```

That's it.

---

# 51. Create a central packet dispatcher

Right now packet logic is deeply nested in `lora_handle_rx_packet()`.

Instead:

```c
void mesh_rx_handle_frame(...)
{
    decrypt;
    parse;
    validate;
    replay;
    dedup;
    mesh_dispatch_packet(&packet);
}
```

Then:

```c
void mesh_dispatch_packet(const mesh_packet_t *packet)
{
    switch (mesh_packet_get_type(packet)) {

    case MESH_PACKET_ACK:
        mesh_handle_ack(packet);
        break;

    case MESH_PACKET_TIME_SYNC:
        mesh_handle_time_sync(packet);
        break;

    case MESH_PACKET_SYNC_REQ:
        mesh_handle_sync_request(packet);
        break;

    case MESH_PACKET_SYNC_RESP:
        mesh_handle_sync_response(packet);
        break;

    case MESH_PACKET_EMERGENCY:
        mesh_handle_emergency(packet);
        break;

    default:
        mesh_handle_unknown(packet);
        break;
    }
}
```

This will eliminate a lot of branching from `app_runtime.c`.

---

# 52. Create a protocol type system

Stop comparing strings everywhere:

```c
strcmp(parsed.type, "ACK") == 0
strcmp(parsed.type, "SYNC_REQ") == 0
```

Parse immediately into:

```c
bems_packet_type_t
```

Then:

```c
if (packet_type == BEMS_PACKET_TYPE_ACK)
```

You already have:

```c
BEMS_PACKET_TYPE_ACK
BEMS_PACKET_TYPE_TIME_SYNC
...
```

Use those enums throughout the application.

---

# 53. Same for priorities

Stop relying exclusively on:

```c
strcmp(priority, "HIGH")
```

Use:

```c
bems_priority_t
```

internally.

Convert:

```text
string ↔ enum
```

only at:

```text
HTTP
JSON
V1 compatibility
```

---

# 54. Create one canonical packet builder

Right now different subsystems construct slightly different representations.

Create:

```c
bool mesh_packet_build(
    const mesh_packet_t *packet,
    uint8_t *out,
    size_t out_size,
    size_t *out_len
);
```

and:

```c
bool mesh_packet_parse(
    const uint8_t *data,
    size_t len,
    mesh_packet_t *packet
);
```

Then:

```text
application
     ↓
mesh_packet_build()
     ↓
V2 serializer
     ↓
crypto
     ↓
radio
```

No subsystem should manually create wire packets.

---

# 55. Make packet ownership explicit

For every packet, define:

```text
CREATED
QUEUED
TRANSMITTING
SENT
FORWARDED
DELIVERED
EXPIRED
DROPPED
```

and a reason:

```text
DUPLICATE
REPLAY
TTL
INVALID
RADIO_FAILURE
QUEUE_FULL
```

This dramatically improves debugging.

---

# 56. Add comprehensive tests that are currently missing

Your existing tests are actually a strong base. You already cover V2, SYNC_RESP, routing, replay, dedup, DNS and message storage.

Add the missing high-value tests.

## TX tests

```text
TX queue accepts message
TX queue rejects duplicate
TX queue full
TX retry 1
TX retry 2
TX retry 3
TX failure after maximum retries
ACK arrives while sending
ACK arrives after timeout
duplicate ACK
wrong source ACK
wrong destination ACK
```

## Forwarding

```text
TTL=0 → no forward
TTL=1 → exactly one forwarding
TTL decrements
duplicate after delay → suppressed
route known → unicast
route stale → fallback flood
broadcast → flood
broadcast duplicate → suppressed
```

## Radio

Mock:

```text
SPI success
SPI failure
TX timeout
TX done
RX CRC failure
invalid version
radio reset
```

## Storage

Most importantly:

```text
save
reboot/load
delete
reboot
delete then reuse slot
power-loss simulation
corrupt record
old record version
full store
```

---

# 57. Add property/fuzz tests for packet parsing

This project is ideal for fuzzing.

Give the parser:

```text
empty packet
1 byte
255 bytes
320 bytes
random bytes
truncated packet
bad lengths
huge lengths
unknown types
unknown priorities
invalid flags
duplicated fields
zero-length fields
```

The only acceptable outcomes are:

```text
valid packet
```

or:

```text
clean false
```

Never:

```text
crash
assert
overflow
out-of-bounds
```

---

# 58. Add fault-injection tests

Test the system as if hardware is failing.

Inject:

```text
NVS failure
NVS full
LittleFS unavailable
SPI timeout
radio absent
WiFi init failure
DNS socket failure
HTTP start failure
malloc failure
queue full
task creation failure
```

The system should degrade gracefully rather than reboot unexpectedly.

---

# 59. Stop using `ESP_ERROR_CHECK()` for recoverable runtime failures

`ESP_ERROR_CHECK()` is acceptable during development for truly fatal initialization failures.

But things like:

```c
httpd_start()
radio
GPIO
NVS
```

should often transition into a controlled degraded state instead of instantly resetting.

Use:

```c
esp_err_t result = ...;

if (result != ESP_OK) {
    ESP_LOGE(...);
    subsystem_mark_failed();
    return;
}
```

Then let the health monitor decide whether recovery/reboot is necessary.

---

# 60. Add a global fault/recovery manager

Create:

```text
src/system/fault_manager.c
```

API:

```c
void fault_report(
    system_fault_t fault,
    esp_err_t error
);

void fault_clear(
    system_fault_t fault
);

bool fault_is_active(
    system_fault_t fault
);
```

Faults:

```text
FAULT_NVS
FAULT_STORAGE
FAULT_RADIO
FAULT_WIFI
FAULT_HTTP
FAULT_MEMORY
FAULT_PROTOCOL
FAULT_WATCHDOG
```

Now all modules have consistent failure semantics.

---

# 61. Improve Wi-Fi/AP handling

Validate:

```text
SSID <= 32
password 8..63 if secured
```

Do not silently allow an invalid AP password.

Create:

```c
esp_err_t wifi_ap_start(...);

esp_err_t wifi_ap_stop(void);

esp_err_t wifi_ap_restart(void);

bool wifi_ap_is_running(void);
```

Currently `wifi_ap.c` is too dependent on `ESP_ERROR_CHECK()`.

---

# 62. Improve DNS server lifecycle

Current DNS task essentially runs forever.

Implement:

```c
dns_server_start();
dns_server_stop();
dns_server_restart();
dns_server_is_running();
```

Use:

```c
shutdown()
close()
```

on failures.

Also add:

```text
socket timeout
```

instead of an indefinite `recvfrom()`.

---

# 63. Improve captive portal behavior

Your DNS interception is present, which is good.

Support the common captive portal checks:

```text
/generate_204
/gen_204
/hotspot-detect.html
/connecttest.txt
/ncsi.txt
```

You already do this.

Add Android/iOS/Windows specific behavior tests and ensure:

```text
DNS query
→ 192.168.4.1
→ HTTP request
→ portal
```

doesn't interfere with your normal API routes.

---

# 64. Improve HTTP server limits

ESP-IDF's HTTP server exposes configurable limits such as `max_open_sockets`, request-header limits, URI limits, receive/send timeouts, and LRU behavior. ([GitHub][3])

Configure explicitly:

```c
config.max_open_sockets = 4;
config.recv_wait_timeout = 5;
config.send_wait_timeout = 5;
config.lru_purge_enable = true;
```

Then tune based on your actual memory budget.

Don't simply use the library defaults and hope they fit the ESP32-S3 workload.

---

# 65. Improve API architecture

Create:

```text
/api/v1/status
/api/v1/messages
/api/v1/nodes
/api/v1/routes
/api/v1/events
/api/v1/health
/api/v1/config
```

Return consistent JSON:

```json
{
    "ok": true,
    "data": {},
    "error": null
}
```

or:

```json
{
    "ok": false,
    "error": {
        "code": "QUEUE_FULL",
        "message": "TX queue is full"
    }
}
```

This is much easier for your frontend.

---

# 66. Add API rate limiting

Especially:

```text
/login
/send
/sync
/settime
```

Rate-limit by:

```text
socket/client
```

and globally.

Example:

```text
/login: 5 attempts / minute
/send: 10 / second
/sync: 2 / 10 sec
```

---

# 67. Make `/send` fully validate messages

Before queueing:

```text
destination valid
type valid
priority valid
payload not empty if required
payload <= PAYLOAD_LEN
destination not local unless supported
ALL allowed only for specific message types
```

Return a specific error:

```json
{
  "ok": false,
  "error": "PAYLOAD_TOO_LARGE"
}
```

---

# 68. Add message expiration

A message should not stay queued forever.

Add:

```c
uint32_t created_epoch;
uint32_t expires_epoch;
```

or monotonic tick equivalent.

Then:

```text
PENDING > 5 min
→ EXPIRED
```

Emergency messages may have different TTLs.

---

# 69. Add route-aware retries

Do not retry exactly the same path three times.

Better:

```text
attempt 1
→ best route

failure

attempt 2
→ second-best route

failure

attempt 3
→ controlled flood
```

This is much more resilient.

---

# 70. Add ACK timeout based on actual link

Don't hard-code:

```c
5000
```

for everything.

Calculate:

```text
ACK_TIMEOUT =
    TX airtime
  + expected propagation
  + network jitter
  + relay delay
```

For this mesh, something like:

```text
base ACK timeout
+
hop count × relay delay
```

will be better.

---

# 71. Add radio airtime awareness

With LoRa, throughput is limited.

The scheduler should know:

```text
packet length
spreading factor
bandwidth
coding rate
```

and estimate airtime.

Then don't queue 20 large low-priority messages ahead of one small emergency packet.

---

# 72. Add a proper emergency queue

Create:

```text
HIGH
NORMAL
BACKGROUND
```

Background traffic:

```text
TIME_SYNC
SYNC_RESP
ROUTE housekeeping
```

should not starve emergency messages.

---

# 73. Separate control packets from application packets

You already have:

```text
ACK
TIME_SYNC
SYNC_REQ
SYNC_RESP
```

Keep them separate internally.

Create:

```c
bool mesh_control_is_control_packet(...);
```

and:

```c
bool mesh_control_send(...)
```

Then application traffic never needs to know how control packets are encoded.

---

# 74. Improve SYNC_RESP behavior

Your SYNC_RESP binary redesign is good.

Add:

```text
maximum records
maximum payload
record count consistency
sender identity validation
timestamp validity
duplicate handling
```

Also don't store synced records without clearly marking their origin:

```c
message.sync_source
message.imported
```

so the UI can distinguish:

```text
RX directly
SYNC imported
```

---

# 75. Add persistent synchronization checkpoints

When a node receives SYNC_RESP, record:

```text
last_sync_peer
last_sync_epoch
last_sync_request
last_sync_success
```

Then synchronize intelligently rather than simply periodically.

---

# 76. Add graceful shutdown/restart sequencing

When factory reset or configuration changes:

```text
stop HTTP
stop DNS
stop TX worker
stop forwarding
stop radio
flush storage
erase config
reboot
```

Do not rely on:

```c
esp_restart();
```

immediately after writing.

---

# 77. Factory reset should erase everything intentionally

Decide exactly what reset means.

At minimum:

```text
node config
messages
route table
roster
sequence state
crypto state
events
```

If the operator expects a true factory reset, do not leave stale mesh state behind.

---

# 78. Add sequence-counter safety

You already persist:

```text
packet_ctr
```

Good.

But define:

```text
reserved block
```

instead of writing NVS every single packet.

For example:

```text
reserve IDs 1000–1999
```

then store:

```text
next reserved range
```

This reduces flash wear.

Example:

```c
#define SEQUENCE_RESERVATION_SIZE 128
```

At reboot:

```text
skip into next reserved block
```

and guarantee uniqueness.

---

# 79. Add NVS wear accounting

Track:

```text
NVS writes
message writes
sequence writes
config writes
```

Expose it in diagnostics.

Your message status currently causes frequent NVS writes, particularly through:

```c
set_status()
```

That's potentially a lot of flash activity.

Use RAM state plus periodic persistence, or only persist important transitions:

```text
QUEUED
SENT
ACKED
FAILED
```

---

# 80. Improve configuration transactions

Current save writes many keys then commits.

Use a versioned config record.

Example:

```c
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    node_config_t config;
    uint32_t crc;
} persisted_config_t;
```

Then:

```text
write new config
→ validate
→ commit
→ boot
```

This makes migration much easier.

---

# 81. Add configuration migration framework

You will eventually have:

```text
config version 1
config version 2
config version 3
```

Create:

```c
esp_err_t node_config_migrate(uint16_t old_version);
```

Never change NVS meanings silently.

---

# 82. Add protocol version handling explicitly

Your V2 parser should return:

```c
MESH_PARSE_OK
MESH_PARSE_BAD_MAGIC
MESH_PARSE_UNSUPPORTED_VERSION
MESH_PARSE_TRUNCATED
MESH_PARSE_INVALID_FIELD
MESH_PARSE_OVERSIZE
```

Instead of only:

```c
bool
```

This helps diagnostics immensely.

---

# 83. Replace Boolean parser returns with error enums where practical

For example:

```c
mesh_parse_result_t parse_mesh_packet_v2(...);
```

Then:

```c
switch (result) {
case MESH_PARSE_OK:
...
case MESH_PARSE_TRUNCATED:
...
}
```

You can still provide a compatibility wrapper:

```c
bool mesh_packet_parse_ok(...)
```

---

# 84. Add centralized limits

Make one configuration file:

```text
include/mesh_limits.h
```

Containing:

```c
#define MESH_MAX_TTL
#define MESH_MAX_PAYLOAD
#define MESH_MAX_NODES
#define MESH_MAX_ROUTES
#define MESH_MAX_MESSAGES
#define MESH_DEDUP_CAPACITY
#define MESH_FORWARD_QUEUE_DEPTH
#define MESH_TX_QUEUE_DEPTH
```

Do not scatter these through the code.

---

# 85. Add compile-time checks

For example:

```c
_Static_assert(PAYLOAD_LEN < 255, "Payload must fit V2 field length");
_Static_assert(MAX_MESSAGES > 0, "MAX_MESSAGES invalid");
_Static_assert(BEMS_MAX_PLAINTEXT <= 255, "Plaintext exceeds LoRa crypto framing");
```

Also verify:

```text
packet buffer
crypto frame buffer
LoRa max payload
```

at compile time.

---

# 86. Make the test suite a real regression gate

The project currently has a good test base, but make it a rule:

```text
Every bug fixed
→ regression test added.
```

Examples:

### Bug:

Message is transmitted twice.

### Test:

```c
test_queue_message_does_not_directly_transmit();
```

### Bug:

TTL not decrementing.

### Test:

```c
test_forwarding_decrements_ttl();
```

### Bug:

Deleted message returns after reboot.

### Test:

```c
test_deleted_message_does_not_reload();
```

### Bug:

Two TX calls collide.

### Test:

```c
test_radio_tx_is_serialized();
```

---

# 87. Add long-running soak tests

This is very important for an ESP32 mesh.

Run:

```text
24 hours
72 hours
7 days
```

with simulated traffic.

Measure:

```text
heap
min heap
largest heap block
task stack
reboots
TX failures
RX failures
duplicate rate
routing stability
NVS writes
```

The node should not progressively degrade.

---

# 88. Add packet fuzzing for at least 100,000 inputs

For:

```c
parse_mesh_packet_v2()
parse_mesh_packet()
decode_sync_response()
```

Generate random byte streams.

Assertions:

```text
never crash
never hang
never write outside buffers
execution remains bounded
```

This will find a large percentage of parser defects.

---

# 89. Add hardware-in-the-loop tests

Once the code is stable, use:

```text
Node A
Node B
Node C
Node D
```

Test:

### Single hop

```text
A → B
```

### Two hop

```text
A → B → C
```

### Three hop

```text
A → B → C → D
```

### Failure

```text
B offline
A → C
```

### Flood

```text
ALL
```

### Congestion

```text
100 messages
```

### Recovery

```text
unplug/replug radio
```

---

# 90. Build a network simulation mode

This would be one of the most valuable additions.

Create:

```text
src/sim/
    sim_radio.c
    sim_network.c
    sim_clock.c
    sim_nodes.c
```

Then simulate:

```text
RSSI
packet loss
duplicates
delays
node failures
route changes
```

This lets you test routing without needing actual LoRa hardware.

---

# 91. Add network fault scenarios

Automate:

```text
20% packet loss
40% packet loss
duplicate packets
out-of-order packets
node failure
radio reboot
route changes
broadcast storms
```

Your mesh should continue operating.

---

# 92. Improve the dashboard around health, not just data

Your UI should make the node's health immediately obvious.

Top section:

```text
NODE ONLINE
RADIO OK
MESH CONNECTED
TIME SYNCED
STORAGE OK
```

Then:

```text
TX queue
RX queue
Routes
Neighbors
Messages
Heap
Uptime
```

---

# 93. Add a node health page

Display:

```text
Node ID
Role
Uptime
Firmware version
Protocol version
Radio state
Frequency
RSSI
SNR
TX count
RX count
Retry count
Replay drops
Duplicate drops
Free heap
Minimum heap
Storage usage
NVS status
```

---

# 94. Add a route visualization

Your existing routing information should become:

```text
LOCAL
 ↓
NODE02  RSSI -58  ACTIVE
 ↓
NODE05  RSSI -72  ACTIVE
 ↓
NODE08  DESTINATION
```

For each route:

```text
Next hop
Metric
RSSI
Age
Success %
```

---

# 95. Add a live event stream

Something like:

```text
20:41:22 RX NODE03 → BRGY001
20:41:22 ROUTE learned: BRGY001 via NODE03
20:41:23 TX ACK → NODE03
20:41:25 SYNC successful
20:41:30 RADIO TX
```

This will make field debugging much easier than reading serial logs.

---

# 96. Add firmware identity

Define:

```c
#define FIRMWARE_VERSION "2.x.x"
#define PROTOCOL_VERSION 2
#define BUILD_ID "..."
```

Expose in:

```text
/api/status
```

and boot log.

Also store:

```text
firmware version
config version
protocol version
```

---

# 97. Add compatibility checks

When receiving:

```text
V2
```

verify:

```text
compatible crypto version
compatible packet version
```

Return:

```text
unsupported
```

rather than trying to parse unknown data.

---

# 98. Add a controlled degraded mode

The node should still provide useful service if one subsystem fails.

Examples:

### LittleFS fails

```text
mesh still works
HTTP falls back to minimal diagnostic
```

### Radio fails

```text
portal works
status says RADIO FAILED
```

### DNS fails

```text
HTTP still accessible by IP
```

### Time sync fails

```text
messages still work
timestamp = unsynchronized
```

This is much better than all-or-nothing startup.

---

# 99. Final architecture I recommend

After the cleanup, the tree should look closer to:

```text
src/
├── main.c
│
├── app/
│   ├── app_init.c
│   ├── app_boot.c
│   ├── app_runtime.c
│
├── mesh/
│   ├── mesh_rx.c
│   ├── mesh_tx.c
│   ├── mesh_forward.c
│   ├── mesh_dispatch.c
│   ├── mesh_protocol.c
│   ├── mesh_retry.c
│   ├── mesh_sequence.c
│   ├── replay_protection.c
│   ├── tx_scheduler.c
│   ├── forward_queue.c
│   ├── mesh_metrics.c
│
├── radio/
│   ├── lora_radio.c
│   ├── lora_tx_queue.c
│   ├── lora_health.c
│
├── routing/
│   ├── route_table.c
│
├── nodes/
│   ├── roster.c
│
├── messages/
│   ├── message_store.c
│   ├── message_repository.c
│
├── storage/
│   ├── storage.c
│   ├── config_store.c
│   ├── message_store_nvs.c
│
├── network/
│   ├── wifi_ap.c
│   ├── dns_server.c
│
├── http/
│   ├── http_server.c
│   ├── http_auth.c
│   ├── http_api.c
│   ├── http_status.c
│   ├── http_messages.c
│   ├── http_setup.c
│   └── ...
│
├── security/
│   ├── crypto.c
│   ├── auth.c
│   └── provisioning.c
│
├── system/
│   ├── watchdog.c
│   ├── health_monitor.c
│   ├── fault_manager.c
│   ├── time_service.c
│   └── factory_reset.c
│
├── led/
│   └── status_led.c
│
└── utils/
    ├── string_utils.c
    └── json_utils.c
```

# 100. The order I would actually implement this

Do **not** randomly improve modules one by one. Use this order.

### Phase 1 — correctness

```text
1. Remove duplicate TX
2. Fix TTL decrement
3. Serialize radio access
4. Fix SX1278 CAD register handling
5. Make SPI/radio errors observable
6. Fix NVS message slot mapping
7. Fix message deletion persistence
8. Remove broad data_mutex
9. Bound forwarding tasks with a queue
10. Fix HTTP request-length handling
```

### Phase 2 — reliability

```text
11. Radio state machine
12. Radio recovery
13. TX state machine
14. Route success/failure scoring
15. Message expiration
16. Storage CRC/versioning
17. NVS wear reduction
18. Health monitor
19. watchdog
20. stack/heap monitoring
```

### Phase 3 — security

```text
21. Remove default production credentials
22. Proper cookie parser
23. Per-client sessions
24. Rate limiting
25. Secure configuration storage
26. Harden provisioning
27. Stronger packet validation
28. AEAD/V3 crypto design
```

### Phase 4 — mesh quality

```text
29. Better route metric
30. Route failure feedback
31. Priority queues
32. Route-aware retries
33. Broadcast optimization
34. Airtime-aware scheduler
35. Better time synchronization
```

### Phase 5 — verification

```text
36. Regression tests for every bug
37. Parser fuzzing
38. Fault injection
39. Long-duration soak tests
40. Four-node hardware tests
41. Packet-loss tests
42. Radio failure/recovery tests
```

### Phase 6 — UI/UX

```text
43. Health dashboard
44. Route visualization
45. Live event log
46. Node diagnostics
47. Queue status
48. Firmware/config information
49. Error explanations
50. Recovery controls
```

## The five changes I would make immediately

Looking specifically at **this uploaded version**, my first five code changes would be:

```text
1. queue_message()
   → REMOVE direct lora_transmit_bytes()

2. delayed_forward_task()
   → decrement packet.hops before forwarding

3. lora_radio.c
   → create one TX queue + one radio-owner TX task
   → never allow arbitrary tasks to transmit directly

4. message_store.c / storage.c
   → replace RAM-index-as-NVS-slot design with fixed persistent slots

5. app_runtime.c
   → split RX parsing/dispatch/forwarding from the runtime coordinator
```

Those five changes will eliminate several of the most serious architectural failure modes before you spend time polishing the UI.

One important point: **“bug-free and perfect” cannot honestly be guaranteed for an embedded mesh system**. The right target is a system where failures are bounded, detectable, recoverable, testable, and where every discovered bug becomes a permanent regression test. That is the standard I would use for this project.

[1]: https://cdn.sparkfun.com/assets/a/a/0/b/5/DS_SX1276-7-8-9_W_APP_V5.pdf?utm_source=chatgpt.com "SX1276/77/78/79"
[2]: https://github.com/espressif/esp-idf/blob/master/docs/en/api-reference/protocols/esp_http_server.rst?utm_source=chatgpt.com "esp-idf/docs/en/api-reference/protocols/esp_http_server.rst at master · espressif/esp-idf · GitHub"
[3]: https://github.com/espressif/esp-idf/blob/master/components/esp_http_server/include/esp_http_server.h?utm_source=chatgpt.com "esp-idf/components/esp_http_server/include/esp_http_server.h at master · espressif/esp-idf · GitHub"
