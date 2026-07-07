# BEMS Feature Tracker

Status legend:
- `done` = already implemented in the codebase
- `todo` = planned, not yet implemented
- `optional` = evidence-gated future work

## Phase 0 - De-duplication / ownership refactor
- `done` Split crypto, mesh protocol, radio, and node config into real modules.
- `done` Moved shared field-length macros into `include/bems_common.h`.
- `done` Fixed the `network_key` layout drift bug in crypto key derivation.
- `done` Removed the duplicate module implementations from `src/main.c`.

## Phase 1 - Remove hardcoded secrets
- `done` Generate the web portal PIN on first boot and persist it to NVS.
- `done` Remove hardcoded fallback secrets from setup flow.
- `done` Enforce a provisioning gate so unprovisioned nodes cannot encrypt, decrypt, or transmit.

## Phase 1b - ID collision check
- `done` Broadcast `ID_CHECK` during setup before saving a Node ID.
- `done` Reply `ID_TAKEN` from already-assigned nodes using jittered replies.
- `done` Block setup save when a collision is detected.

## Phase 2 - WiFi hardening
- `todo` Switch the AP to WPA2 with an operator-managed shared credential.
- `todo` Add login lockout with escalating delay.
- `todo` Rotate session tokens per login and expire them after inactivity.

## Phase 3 - Reliability split
- `todo` Generalize sender-side ACK/retry tracking for all unicast priorities.
- `todo` Add broadcast repeat logic with jitter.
- `todo` Surface explicit delivery status for all messages.

## Phase 4 - Buffer eviction + dedup scaling
- `todo` Prefer evicting completed messages before active high-priority ones.
- `todo` Replace linear seen-packet scans with hash-based lookup.
- `todo` Add the Phase 5 roster-eviction hook stub.

## Phase 5 - Roster
- `todo` Add passive roster learning from live mesh traffic.
- `todo` Add `last_hop` to the mesh packet contract.
- `todo` Add `/api/roster` and roster UI.
- `todo` Persist roster state to NVS with debounced writes.

## Phase 6 - Route table
- `todo` Learn routes passively from roster data.
- `todo` Add route cost comparison and fallback flood behavior.

## Phase 6b - WHO_ONLINE discovery
- `todo` Add on-demand WHO_ONLINE discovery and ONLINE_ROSTER aggregation.

## Phase 7 - Config wizard
- `todo` Add the first-boot wizard for identity, location, defaults, and advanced options.

## Phase 8 - Smart suppression flooding
- `optional` Only add if real mesh testing shows flooding problems.
_____________________________________________________________________________________________________________________________








# BEMS Firmware — Copy/Paste Prompt Pack for Your AI Coder

How to use this file:
- Paste **Block 0 (context + guardrails)** once, at the very start of your session with the AI coder.
- Then paste **one phase block at a time**, in order (0 → 8), waiting for each phase to be fully
  finished, reviewed, and merged before pasting the next.
- Do not paste multiple phase blocks in one message. The guardrails in Block 0 exist specifically to
  stop the AI from "getting ahead" — pasting phases out of order defeats that.
- Where a phase requires a decision from you (marked **DEFAULT:**), the default is filled in so you
  can proceed without stopping. Change it inline before pasting if you want something else.

---

## BLOCK 0 — Paste this first (context + rules, applies to every phase below)

```
You are implementing a multi-phase refactor of an ESP32 + LoRa mesh emergency messaging
firmware (BEMS — barangay/community emergency mesh). This is a real embedded C project
(PlatformIO/ESP-IDF), not a toy exercise. I will paste one phase at a time. Read this
whole block before starting Phase 0.

PROJECT LAYOUT (confirmed from source):
- src/main.c            (2712 lines — currently contains a full duplicate implementation
                          of everything below; this is the core bug the project is built
                          around and Phase 0 fixes it)
- src/bems_crypto.c     (241 lines — the ONE module that is real, compiled, and linked;
                          everything else below is currently a header-only "sketch" the
                          build never actually calls)
- include/bems_crypto.h
- include/mesh_protocol.h  (defines MAX_SEEN_PACKETS, mesh_packet_t contract)
- include/lora_radio.h     (declares extern spi_device_handle_t lora_spi — a shared radio
                          module main.c was clearly meant to call but doesn't)
- include/node_config.h    (defines node_config_t)
- platformio.ini / CMakeLists.txt

GROUND TRUTH — already verified against the actual source, trust it, don't re-derive it:
1. main.c:498-721 duplicates bems_crypto.c almost line-for-line (real crypto duplication —
   two live implementations exist side by side, unlike the other modules which are dead code).
2. main.c:796-921, 1511-1554 duplicate mesh_protocol.h's contract (parse_mesh_packet,
   build_forward_packet, packet_seen/remember_packet).
3. main.c:147-155, 1809-1943 duplicate node_config.h (node_config_t, load/save/defaults).
4. main.c:449-496, 722-1300, 1406+ duplicate lora_radio.h (all lora_* / SX1278 register work).
5. FIELD_LEN, SITIO_LEN, BARANGAY_LEN, MUNICIPALITY_LEN are #defined identically in THREE
   places (node_config.h, mesh_protocol.h, main.c). They agree today but nothing enforces it.
6. KNOWN DRIFT BUG (fix as part of Phase 0, don't just note it): bems_crypto.c line 9 does
   `extern struct { char network_key[32]; } node_config;` — assuming network_key is the
   FIRST member at offset 0. The real node_config_t (node_config.h) has network_key as the
   LAST member, after configured/node_id/node_name/location/default_destination/web_pin.
   If bems_crypto.c's derive_crypto_keys() were ever linked against the real global instead
   of main.c's private inline copy, it would silently read the wrong bytes as the key —
   no crash, just wrong-key derivation. This is the exact kind of drift Phase 0 exists to
   catch and must be corrected, not preserved.
7. Hardcoded secrets are live fallback paths, not placeholder text: setup_handler()
   (main.c:2322-2327) fills in DEFAULT_WEB_PIN "1234" / DEFAULT_NETWORK_KEY "CHANGEME1234567"
   whenever the operator leaves those fields blank.
8. AP_PASSWORD is a compile-time "" (main.c:207); start_wifi_ap() (main.c:2650-2668) only
   upgrades past WIFI_AUTH_OPEN if strlen(AP_PASSWORD) >= 8 — so the AP is always open today.
9. session_token (main.c:211) is generated once at boot by init_session_token() (main.c:2212)
   and reused for every login forever — no rotation, no expiry.
10. login_handler() (main.c:2258) has zero rate limiting.
11. retry_tracker_add_by_value() (main.c:1582) explicitly returns early unless
    priority == "HIGH" and destination != "ALL". IMPORTANT: the receiver side already
    unconditionally ACKs any non-ACK unicast addressed to it, regardless of priority
    (main.c:1376-1378, send_ack_packet fires whenever is_for_me && !is_ack). So the
    wire-level ACK already exists for all unicast priorities — Phase 3 is a SENDER-side
    tracking + UI-status generalization, not a new receiver-side mechanism. Don't touch
    the receiver ACK logic.
12. next_message_slot() (main.c:781) does an unconditional FIFO memmove-shift when
    MAX_MESSAGES=16 is full, regardless of ACKED/FAILED/still-active status.
    packet_seen()/remember_packet() (main.c:796-855) do a linear scan over
    MAX_SEEN_PACKETS=64.

MERGED DESIGN DECISION (read before Phase 5 — do not build the roadmap's original
"neighbor table" and a separate "roster" as two systems):
- Build ONE table, called `roster[]`, not a separate "neighbor table" — "neighbor table"
  wrongly implies only direct radio neighbors; this table holds multi-hop-reachable nodes
  too, which routing (Phase 6) needs anyway.
- Do NOT add a new periodic HELLO beacon packet. Populate the roster passively by tagging
  a `last_hop` field onto packets that already flow through the mesh and updating the
  roster on every successful parse_mesh_packet() in the RX task — zero new airtime. Add
  on-demand WHO_ONLINE discovery rounds for a confident complete snapshot when the
  operator wants one. Airtime is the scarce resource here; don't spend it on redundant
  beacons when passive learning gets equivalent data for free.
- Use ONE staleness/three-state model (online / stale / never-heard) everywhere it's
  needed: the UI, the Phase-4 buffer-eviction hook, and the Phase-6 routing fallback.
  Don't invent a second staleness threshold anywhere.
- Full detail on this merge is in Phase 5's block below — don't jump ahead to it now.

UNIVERSAL RULES — these apply to every phase, not just the one currently in front of you:

1. ONE PHASE AT A TIME. Do not start next-phase work inside the current phase's response,
   even if it looks convenient or "while I'm already touching this file." If a later
   phase's need becomes obvious mid-work, add a clearly-named stub and a comment
   (e.g. `// TODO(Phase 5): invalidate retry entry when roster[destination] goes stale`)
   and say so in prose — do not implement it early.
2. NO SCOPE CREEP. Implement exactly what the current phase's prompt asks for. If you
   notice something else that looks broken or improvable, tell me in your response —
   do not just fix it in the same diff.
3. NEVER LEAVE TWO IMPLEMENTATIONS OF THE SAME LOGIC ALIVE AT ONCE, even "temporarily for
   safety." When migrating a function out of main.c into its real module (Phase 0), delete
   the main.c copy in the SAME change that wires in the call to the real function. A
   half-migrated function with both versions present is worse than not migrating it yet —
   it silently reintroduces the exact drift bug in Ground Truth #6.
4. GREP EVERY CALL SITE before finishing any phase that touches mesh_packet_t,
   node_config_t, or any packet-builder function. main.c has ~10 different
   snprintf-based packet builders; missing one when a field is added produces a packet
   that silently parses wrong instead of failing loudly. List the call sites you checked
   in your response.
5. STATE ASSUMPTIONS BEFORE WRITING CODE, not after. If a phase has an explicit open
   decision (wire compatibility, chunking strategy, persistence strategy), say which
   option you're taking and why, in your first paragraph, before the diff.
6. DO NOT TOUCH RADIO TIMING CONSTANTS (LORA_SPREADING_FACTOR, LORA_MODEM_CONFIG_*,
   frequency) unless the current phase explicitly calls for it. These are
   regulatory/interop-sensitive. Phase 7 only exposes EXISTING presets as UI — it does
   not invent new radio parameters.
7. PRESERVE the existing wire format's `BEMS|...|` pipe-delimited structure and the
   jittered-reply pattern (`vTaskDelay(200 + esp_random() % 1001)` before replies) in
   any packet you touch or add. Reuse these patterns, don't redesign them.
8. TESTING CONSTRAINT: I have 2 physical nodes right now, not 3+. Any logic needing
   3-node path competition (Phase 6's cost comparison) must be written as pure,
   hardware-independent code, unit-testable in the native PlatformIO env with synthetic
   RSSI/hop-count fixtures. Don't defer this by saying "test it later on real hardware."
9. If you're unsure whether something belongs in the current phase, ASK — don't guess.
   A wrong guess is either scope creep (rule 2) or a half-built phase, both worse than
   a question.
10. At the end of each phase, append one line to `docs/DECISIONS.md` (create it in
    Phase 0 if it doesn't exist) recording any open-question choice you made and why.
    This prevents decisions made in an early phase from silently conflicting with a
    later one.

Confirm you've read and understood this before I paste Phase 0.
```

---

## PHASE 0 — De-duplication / ownership refactor

```
PHASE 0 of 8 (+1b, +6b). De-duplicate main.c against the real modules and fix the known
drift bug, per the Ground Truth and Universal Rules already given.

1. Refactor so main.c no longer contains inline reimplementations of logic that belongs
   in bems_crypto.c, mesh_protocol.h, lora_radio.h, and node_config.h.
2. First, check platformio.ini/CMakeLists.txt to confirm which of these are actually
   compiled today (bems_crypto.c should be; mesh_protocol.h/lora_radio.h/node_config.h
   likely have no corresponding .c file and are currently pure headers with no build
   presence). Tell me what you find before changing anything.
3. Give mesh_protocol.h, lora_radio.h, and node_config.h real .c implementation files if
   they don't have one, containing the logic currently duplicated in main.c.
4. Each module owns its own state as static/private data, exposed only through get/set
   or action functions it defines — no bare externs for node_config_t, messages[],
   seen_packets[], retry_entries[], or data_mutex.
5. Move data_mutex into the module that owns the data it protects, with a lock/unlock
   function pair local to that module.
6. Migrate ONE function at a time: for each one, delete the main.c copy in the same
   change you wire in the call to the real module function. Never leave both versions
   present (Universal Rule 3).
7. Fix the known drift bug (Ground Truth #6): bems_crypto.c's derive_crypto_keys() must
   read network_key from the real node_config_t layout (last member), not assume offset
   0. Show me the before/after of this specific fix.
8. Collapse the triplicated FIELD_LEN / SITIO_LEN / BARANGAY_LEN / MUNICIPALITY_LEN
   #defines (currently in node_config.h, mesh_protocol.h, and main.c) into ONE shared
   header, included by all three. Also move the inconsistent MIN(a,b)/MAX(a,b) macros
   (bems_crypto.c has MIN, main.c's build_forward_packet uses MAX) into that same shared
   header so neither gets redefined a third time later.
9. main.c should end up only doing: task/thread creation, HTTP handlers, and wiring
   modules together — no domain logic.
10. Call out anywhere ELSE you find main.c's copy had already drifted from the module
    version (beyond the known bug in #7) and tell me which behavior is correct before
    merging.
11. Create docs/DECISIONS.md and log this phase's structural choices (e.g. where each
    module's .c file lives, mutex placement).
```

---

## PHASE 1 — Remove hardcoded secrets

```
PHASE 1 of 8 (+1b, +6b). Depends on Phase 0 (single-copy modules) being merged.

Remove all hardcoded default secrets (DEFAULT_NETWORK_KEY, DEFAULT_WEB_PIN — currently
"CHANGEME1234567" and "1234", used as live fallbacks in setup_handler() whenever fields
are left blank). Implement:

1. Web portal PIN: generate randomly using esp_random() on first boot, store to NVS
   immediately, never compile in a literal PIN string.
2. Network key: implement a pairing flow — the first node in a deployment generates a
   random key via esp_random() and displays it once (serial console or a one-time
   pre-AP setup page); every other node accepts the key as manual input during setup.
   No node should ever have a compiled-in network key value.
3. Add a real "provisioned" gate: extend node_config.configured so that mesh TX/RX
   (encryption, decryption, radio operation) is refused outright if the network key
   still equals the placeholder value — not just a UI reminder. Show me exactly where
   in the radio/crypto call path (post-Phase-0, this is now the real module boundary,
   not main.c's inline copy) this gate needs to be enforced so there's no bypass.
4. Log your choice in docs/DECISIONS.md, including where exactly you placed the gate.
```

## PHASE 1b — ID collision check (new — from the mesh discovery design)

```
PHASE 1b of 8. Depends on Phase 0. Pairs with Phase 1 — both are first-boot
identity/trust setup, so do this right after Phase 1.

Implement an ID_CHECK / ID_TAKEN setup-time collision probe, per the mesh discovery
design §3.3:

1. When an operator enters/confirms a Node ID during setup, broadcast an ID_CHECK
   control packet containing the proposed ID before allowing the save to commit.
2. Any other node on the mesh already using that ID replies ID_TAKEN (use the existing
   jittered-reply pattern: vTaskDelay(200 + esp_random() % 1001) before replying, per
   Universal Rule 7).
3. If no ID_TAKEN reply arrives within a reasonable timeout, allow the save. If one
   arrives, block the save and tell the operator the ID is in use.
4. This gates setup_handler()'s save path — do not let a node save an ID without this
   check completing (or timing out) first. This will also gate the Phase 7 wizard's
   Node ID field later; note that dependency in a comment now.
5. Log the timeout value you chose and why in docs/DECISIONS.md.
```

---

## PHASE 2 — WiFi hardening

```
PHASE 2 of 8. Depends on Phase 1.

Harden the WiFi AP and portal login:

1. Switch from AP_PASSWORD="" (currently always WIFI_AUTH_OPEN because
   strlen(AP_PASSWORD) < 8 at main.c:2666-2668) to WPA2, using a passphrase distributed
   the same way as the Phase 1 network-key pairing (operator-managed shared credential).
   Make clear in code/comments that this password is DISTINCT from the portal PIN.
2. Add login lockout: track failed PIN attempts per session/IP, escalating delay
   (3 fails -> 30s lock, doubling per further failure), state persisted at least for
   the session's lifetime. login_handler() (main.c pre-refactor line 2258, now in its
   real module) currently has zero rate limiting — this is the gap being closed.
3. Replace the single static session_token (currently generated once at boot by
   init_session_token() and reused forever) with a fresh token issued per successful
   login, expired after a period of inactivity (pick a sane default, make it
   configurable).
4. Log the lockout curve and token-expiry default you chose in docs/DECISIONS.md.
```

---

## PHASE 3 — Reliability split

```
PHASE 3 of 8. Depends on Phase 0 (independent of routing — do this before Phase 5/6
since routing changes *how* packets are sent, unicast vs flood).

Split message reliability into two correct models. Note: the receiver side already
unconditionally ACKs any non-ACK unicast packet addressed to it, regardless of
priority (send_ack_packet fires whenever is_for_me && !is_ack) — do NOT touch that.
This phase is sender-side tracking + UI status only.

1. Unicast (any priority): generalize the existing ACK/retry mechanism
   (retry_tracker_add_by_value, which currently returns early unless
   priority == "HIGH" && destination != "ALL") to cover all priorities, but tune
   attempts/backoff per priority (e.g. HIGH: more attempts, tighter backoff; LOW: fewer
   or none) instead of a hard HIGH-only cutoff.
2. Broadcast ("ALL" destination): do NOT add ACK tracking to broadcasts. Instead send a
   small number of repeats with randomized jitter, relying on redundant reception across
   neighbors as the delivery guarantee.
3. Surface real delivery status to the operator/UI for every message: "SENT (no
   delivery confirmation expected)" for broadcasts/low-effort unicast, "ACKED", or
   "FAILED" — so the current invisible gap for non-HIGH messages becomes visible truth.
4. Log the per-priority attempt/backoff table you chose in docs/DECISIONS.md.
```

---

## PHASE 4 — Buffer eviction + dedup scaling

```
PHASE 4 of 8. Depends on Phase 0.

Fix the fixed-size buffer behavior:

1. Message table (MAX_MESSAGES=16, currently evicted by an unconditional FIFO memmove
   in next_message_slot() regardless of status): implement an eviction policy that
   never silently drops an active, unacknowledged HIGH-priority message. Evict
   completed (ACKED/FAILED) entries first, oldest first. If the table is full of
   still-active important messages, surface a "queue full" state to the operator
   instead of silently overwriting.
2. Seen-packet dedup table (MAX_SEEN_PACKETS=64, 60s TTL, currently a linear scan in
   packet_seen()/remember_packet()): replace the linear scan with a hash-based lookup
   keyed on (source, packet id) so lookup cost stays flat as the table grows. Recommend
   a sensible default size based on expected traffic volume x current TTL.
3. Add a stub hook for tying message-table eviction to roster staleness (this depends
   on Phase 5's roster, which doesn't exist yet) — do NOT implement it now, just add:
   `// TODO(Phase 5): invalidate retry entry when roster[destination] goes stale`
   at the relevant call site, per Universal Rule 1.
4. Log the dedup table size/TTL math in docs/DECISIONS.md.
```

---

## PHASE 5 — Roster (passive learning, replaces the roadmap's HELLO-beacon design)

```
PHASE 5 of 8 (+1b, +6b). Depends on Phase 0 (needs the single-copy protocol module for
mesh_packet_t). Ship this alone; it's useful standalone and de-risks the roster data
structure before Phase 6 builds on it.

Do NOT build a periodic HELLO beacon (an earlier draft of this project considered one —
it's superseded by this design). Build passive learning instead, per the merged design
in Block 0.

1. Build ONE structure, roster[], not a separate "neighbor table":
   ```c
   typedef struct {
       char node_id[FIELD_LEN];
       location_info_t location;
       char next_hop[FIELD_LEN];      // = last_hop of the best packet heard from this node
       int hops;                      // smallest HOPS-remaining seen (distance proxy)
       int rssi;
       int snr;
       TickType_t last_seen_tick;
       bool learned_passively;        // false once confirmed via active WHO_ONLINE reply
   } roster_entry_t;
   ```
2. Add a `char last_hop[FIELD_LEN]` field to mesh_packet_t in the (now single-copy, post
   Phase 0) protocol module. This bumps the wire field count from 10 to 11.
   IMPORTANT — wire compatibility: DEFAULT (flag this to me, don't silently pick this
   permanently): assume a coordinated flag-day upgrade (all nodes updated together) for
   now, since the fleet is small during development. Add a one-line comment marking this
   assumption and bump BEMS_CRYPTO_VERSION so a version mismatch is at least detectable
   later, but do not build full mixed-version support in this phase — that's a bigger
   decision I'll make explicitly before any gradual-fleet-upgrade scenario is real.
3. Update EVERY packet-building snprintf that constructs mesh_packet_t's wire format to
   include the new field — parse_mesh_packet's fields[10] array and field_count < 10
   check becomes fields[11]/< 11, plus build_forward_packet, send_ack_packet,
   build_packet, and any other control-packet builders. Grep and list every call site
   you updated (Universal Rule 4).
4. On every successful parse_mesh_packet() in the RX task, update
   roster[source_node_id] using the packet's last_hop, hop count, RSSI, and SNR — no
   new airtime spent, this rides on existing traffic.
5. Age entries using a three-state model: online / stale / never-heard. After N missed
   traffic windows (pick a sane default consistent with existing timing patterns in the
   codebase), mark stale; after a further period, drop. This is THE ONE staleness model
   — Phase 4's stub hook and Phase 6's routing will both reuse it, don't invent another.
6. Add a GET /api/roster endpoint and wire it into a "Mesh Health" UI section showing
   node_id, last hop, hops, rssi/snr, and online/stale/never-heard status.
7. Testability: I have 2 physical nodes right now. Structure the aging/expiry logic so
   I can validate it by powering one node off and confirming the other marks it stale
   then drops it.
8. Roster persistence across reboot: DEFAULT — persist to NVS, following the existing
   debounced-write pattern already used for packet_counter (only write on actual
   change, not on every update, to avoid flash wear). Flag if you think
   rebuild-from-scratch-each-boot is better for this codebase and why, but implement
   the NVS-persisted default unless I say otherwise.
9. Log the staleness thresholds, NVS write-debounce interval, and version-byte bump in
   docs/DECISIONS.md.
```

---

## PHASE 6 — Route table (passive routing on top of the roster)

```
PHASE 6 of 8. Depends on Phase 5 (roster). Do not implement a separate
route-discovery/RREQ-RREP protocol — this reuses roster[] as its data source.

1. Learn routes passively: whenever a node relays or receives a flooded packet, record
   "the source of this packet is reachable via the neighbor that just delivered it, at
   (their hop count + 1)" — this is really just interpreting roster[] entries as routes,
   not a second table.
2. Cost metric: cost = hops + penalty(rssi/snr below threshold) per hop. A coarse
   3-tier good/marginal/bad link bucket is enough — do not implement full ETX math.
3. Freshness/loop guard: reuse the existing per-message sequence/packet ID as the
   route's freshness marker. Discard a "route" heard via the same neighbor with an
   older sequence number than what's already stored.
4. Expiry: reuse roster[]'s three-state staleness model from Phase 5 — the instant a
   route's next-hop roster entry goes stale, invalidate every route depending on it
   immediately. Do not invent a second staleness timer for routing.
5. Sending logic — this is the critical rule: if a known-good route exists, send
   unicast directly to the next hop. If the route is unknown, stale, or the unicast
   fails to get an ACK after a couple tries, fall back to full flood for that message,
   and let the failure invalidate the bad route entry. Never let "I have a route"
   become "I will only try that one path" — flood is always the safety net, routing is
   only an optimization on top of it.
6. Write the cost-comparison logic (given two competing neighbor/hop combos, pick the
   better path) as pure, hardware-independent code, unit-testable in the native
   PlatformIO env with synthetic RSSI/hop-count fixtures (I only have 2 physical nodes,
   can't test real 3-node path competition yet — per Universal Rule 8).
7. Log the cost-bucket thresholds and the wire-compatibility approach actually shipped
   in docs/DECISIONS.md.
```

## PHASE 6b — WHO_ONLINE active discovery (new — from the mesh discovery design, can ship parallel to or right after Phase 6)

```
PHASE 6b of 8. Depends on Phase 5 (roster). Independent of Phase 6's routing logic.

Implement on-demand active discovery, per the mesh discovery design §3.2:

1. Add WHO_ONLINE / WHO_ONLINE_REPLY / ONLINE_ROSTER control packet types.
2. On operator request (a /discover endpoint + refresh button in the UI), broadcast
   WHO_ONLINE with a HOPS budget. DEFAULT hop budget: pick a number consistent with
   your expected deployment size (e.g. 3-5 for a barangay-scale mesh); flag this number
   to me explicitly rather than silently deciding — it trades discovery reach against
   probe airtime cost.
3. Nodes receiving WHO_ONLINE reply with WHO_ONLINE_REPLY using the existing jittered
   pattern (vTaskDelay(200 + esp_random() % 1001)) to avoid a reply storm.
4. Aggregate replies into an ONLINE_ROSTER response for the requesting operator's UI.
   Chunking strategy for ONLINE_ROSTER: DEFAULT — size-based chunking (split by payload
   byte budget, not by a fixed node count), since this is more robust to long node
   IDs/location strings than count-based chunking. Flag if you think count-based is
   simpler to implement correctly here and why.
5. Mark roster[] entries confirmed via WHO_ONLINE_REPLY as learned_passively = false.
6. Log the hop budget and chunking strategy actually shipped in docs/DECISIONS.md.
```

---

## PHASE 7 — Config wizard

```
PHASE 7 of 8. Depends on Phases 1, 1b, 2, 5, 6 (needs final field set: node role from
routing/roster work, radio presets, and the ID collision check must gate the Node ID
field).

Build a first-boot configuration wizard for the web portal, covering these fields (mark
which already exist in node_config_t vs. are new):

Section order:
1. Network membership (blocking, cannot be skipped):
   - Join existing network vs. create new network (drives the Phase 1 key pairing flow)
   - Network key
   - Web portal PIN
   - WiFi AP password (new, from Phase 2's WPA2 change) — UI must make clear this is a
     different credential from the portal PIN
2. Identity:
   - Node ID (suggest MAC-derived default, editable to something like
     BRGY-SANISIDRO-01) — MUST run the Phase 1b ID_CHECK collision probe before
     allowing save
   - Node name/display label
   - Node role (new): relay-only / relay+message-origin / gateway
3. Location:
   - Sitio / Barangay / Municipality — require a confirmation step to change after
     first save, since a typo here silently mislabels all future messages from this node
4. Messaging defaults:
   - Default destination
   - Default priority for new messages (new)
5. Advanced (collapsed section — deliberately minimal, see rationale below):
   - TX power ceiling (optional, for battery saving)
   - Low-battery threshold
   - Range Profile (optional, only if you decide you want it): a single Standard /
     Long Range toggle — NOT raw SF/BW/CR. Internally this maps to a fixed pair of
     complete radio profiles (e.g. Standard = SF9, Long Range = SF12), never
     individually adjustable fields.
   - Factory reset

   DO NOT expose: frequency, spreading factor, bandwidth, coding rate, or time-sync
   source as individually adjustable fields. This hardware (Ai-Thinker RA-02 / SX1278,
   433 MHz, fixed single-band deployment) has no multi-region or per-node need for
   these to vary, and letting operators change them independently is a real
   interoperability hazard: if Barangay A changes SF/BW and Barangay B doesn't, the
   two nodes silently stop being able to talk to each other, with no obvious error to
   the operator. Compile ONE radio profile into firmware as the default (e.g. 433 MHz,
   125 kHz BW, CR 4/5, SF10, CRC on, fixed sync word) that every node ships with
   identically. If the Range Profile toggle above is implemented, it must swap between
   two complete, internally fixed profiles — never expose the underlying parameters
   themselves in the UI. This still respects Universal Rule 6 (don't touch radio
   timing constants unless the phase calls for it) — the toggle changes which
   pre-baked profile is active, it does not add new tunable parameters.

Wizard mode: DEFAULT — progressive (node works with safe temporary defaults but nags
persistently until security-critical fields — network key, PIN, AP password, Node ID —
are set), rather than linear-blocking. Flag this back to me if you think blocking is
actually safer for this deployment context before building it, but implement
progressive unless I say otherwise.

Log the final wizard mode and field defaults in docs/DECISIONS.md.
```

---

## PHASE 8 (optional, do not schedule speculatively) — Smart-suppression flooding

```
PHASE 8 — OPTIONAL. Depends on Phase 6. Do NOT implement this unless real-world testing
of the deployed mesh shows actual broadcast storms / redundant-flood problems. This
phase exists only as a placeholder for a documented, evidence-gated future
optimization — do not start it as part of the current implementation pass. If asked to
start Phase 8, first ask what evidence of a flooding problem prompted it.
```

---

## Quick reference — open questions and the defaults baked into the prompts above

| # | Question | Default used above | Where to change it |
|---|---|---|---|
| 1 | Wizard mode | Progressive with nagging | Phase 7 block |
| 2 | HELLO beacon vs passive-only roster | Passive-only + on-demand WHO_ONLINE (beacon dropped entirely) | Phase 5 block |
| 3 | Wire compatibility (10→11 fields) | Flag-day upgrade assumed for now, version byte bumped for future detection | Phase 5 block |
| 4 | ONLINE_ROSTER chunking | Size-based | Phase 6b block |
| 5 | Roster persistence across reboot | NVS-backed, debounced writes | Phase 5 block |
| 6 | WHO_ONLINE hop budget | Flagged for you to confirm a real number (3-5 suggested) | Phase 6b block |
| 7 | Radio parameters in wizard | Fixed in firmware (one compiled profile, e.g. 433 MHz/125 kHz BW/CR 4/5/SF10); at most a Standard/Long-Range toggle over two pre-baked profiles — no per-field SF/BW/CR/frequency exposed | Phase 7 block |

If any of these defaults are wrong for your deployment, edit that line in the relevant
phase block before pasting it.

Note on #7: this isn't just a UI simplification — it's an interoperability constraint.
Letting individual nodes change SF/BW/CR independently means two barangays can silently
drift onto incompatible radio settings with no error shown to either operator. Locking
one profile per firmware build (or at most a binary Standard/Long-Range toggle over two
complete pre-baked profiles) keeps every node in a deployment mutually compatible by
construction, which is the stronger answer if a thesis panel asks why operators can't
tune the radio.