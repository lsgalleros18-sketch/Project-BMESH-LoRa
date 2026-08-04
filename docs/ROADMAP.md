# Feature Roadmap

The implemented features are documented in [FEATURES.md](./FEATURES.md).

## Remaining Roadmap

- Phase 2: WiFi hardening
- Phase 4: Buffer eviction + dedup scaling
- Phase 8: Smart suppression flooding

## Notes

- Phase 0, Phase 1, Phase 1b, and Phase 3 are already represented in the shipped codebase and belong in the feature catalog.
- The remaining phases are planning items and may change as implementation progresses.

## UI/UX Improvement Spec

The following portal UI/UX guidance was provided for the BMESH-LoRa project and should be treated as implementation notes for future portal work:

- The portal is a self-contained ESP32 captive web UI with no CDN, no external fonts or libraries, and no build step.
- All pages should remain single-file documents using inline HTML/CSS/JS inside `src/main.c`.
- The audience is barangay-level emergency responders using low-end phones on a weak local AP, so the UI should prioritize low cognitive load, high glanceability, and minimal tap friction.
- Any future UI changes should preserve the existing no-framework, inline-assets approach and avoid adding new images or remote dependencies.

For the full page-by-page issue list, refer to the imported spec attached to this task.




# BMESH-LoRa Portal — UI/UX Improvement Spec

## Context (read this before implementing anything)

This is **not** a normal web app. The UI is three `static const char` HTML/CSS/JS
strings baked directly into `src/main.c` (`INDEX_HTML`, `SETUP_HTML`,
`LOGIN_HTML`), served by an ESP32 over a local Wi-Fi AP with **no internet
access**. It is a barangay (Philippine village-level) emergency response tool —
used by non-technical local officials, sometimes under time pressure, on
mid-range Android phones, over a weak local AP signal.

Hard constraints any change must respect:
- **No CDN, no external fonts/libraries, no build step.** Everything must be
  inline in the C string (or a tiny vendored file) — flash/SPIFFS space is
  limited and there's no network to fetch anything from.
- **No frameworks.** Vanilla HTML/CSS/JS only, matching the existing style.
- Keep each page a **single self-contained document** — that's the existing
  pattern and it should stay that way.
- Every added byte costs flash. Prefer CSS/JS reuse over new assets. No new
  images — use inline SVG or CSS shapes for any icons.
- The audience may not be fluent readers of dense English UI text, may be
  operating this while stressed (active flood/fire/evacuation), and may be
  using a cracked/low-brightness screen outdoors. Design for **low
  cognitive load and high glanceability**, not visual polish for its own sake.

Below, issues are grouped by page, with **Issue → Why it matters → Fix**, and
tagged by priority. Feed this to your coding agent section by section —
each item references the actual `id`/`class` names already in `src/main.c` so
it can locate the code.

---

## Priority Legend
- **P0** — safety/reliability impact (operator could lose a message, miss a
  warning, or damage the node) — fix first.
- **P1** — significant usability friction during real incident use.
- **P2** — polish / nice-to-have, do after P0/P1.

---

## 1. Cross-cutting issues (apply to all three pages)

### 1.1 [P0] Silent actions give no feedback
`/settime`, `/sync`, and `/reset` (index page, lines ~352–356) are plain
`<form method=post>` submissions that trigger a full page reload with **no
success/failure feedback** — unlike `/send` and `/ota`, which are
intercepted by JS and show a toast. This is inconsistent and risky: an
operator clicking "Sync Messages from Mesh" during an incident has no idea
whether it worked.

**Fix:** Intercept all four action forms (`send`, `settime`, `sync`, `ota`,
`reset`) the same way, through one shared `postAction(url, formOrBody,
{successMsg, failMsg})` helper that always shows a toast and never causes a
full navigation/reload except where a redirect is intentional (setup save,
factory reset).

### 1.2 [P0] Toasts disappear too fast, and failures look identical to successes
Toasts auto-hide after 1.8–2.2s (`setTimeout(...,1800/2200)`) with the same
dark style regardless of outcome. Under stress, a user may miss a fast-fading
failure toast and assume a message went out when it didn't.

**Fix:**
- Color-code toasts: green/neutral for success, **red** for failure, with a
  persistent (not-timed) failure toast that requires a tap to dismiss, or at
  minimum 5–6s duration.
- Add a small icon (✓ / !) so meaning is readable at a glance without reading
  text.

### 1.3 [P0] Destructive action ("Factory Reset") uses a native `confirm()`
Both `INDEX_HTML` and `SETUP_HTML` wire `onsubmit='return confirm(...)'` for
factory reset. Native confirm dialogs are easy to reflexively accept
(muscle-memory "OK" tap) and give no context of what's about to be lost.

**Fix:** Replace with a custom in-page confirmation step: clicking "Factory
Reset Node" reveals a red-bordered panel that requires typing the Node ID (or
tapping a second explicit "Yes, erase this node" button) before the POST
fires. Make it visually distinct from the primary action ("danger zone"
styling: red outline, warning icon, extra spacing so it can't be fat-fingered
next to Save/Sync buttons).

### 1.4 [P1] Fragile, incomplete i18n implementation
The English/Tagalog toggle (`strings.en`/`strings.tl`, `applyLang()`) selects
elements by **DOM position** — e.g.
`document.querySelectorAll('label')[2].childNodes[1].textContent = ...` and
`document.querySelectorAll('section.card h3')[1]`. This breaks silently (wrong
label gets translated, or none does) the moment markup order changes, and it
only covers a handful of static strings — dynamic content (status line,
message bubbles, health/route panels, warnings, button labels like "Sync
Clock", "Factory Reset Node") is **never translated**.

**Fix:** Add `data-i18n="key"` attributes to every translatable element and
drive `applyLang()` off a `document.querySelectorAll('[data-i18n]')` loop that
looks up `strings[lang][key]`, not array index. Extend the dictionary to
cover every static label on the page (buttons, section headers, portal
footer text, warning strings, placeholders via `data-i18n-placeholder`).
Persist the chosen language in `localStorage` (fine for this — it's just a UI
preference, not app state) so it doesn't reset to English every page load.

### 1.5 [P1] No visible connectivity/staleness state
`load()` polls every 15s and on failure just replaces `#health`/`#routes`
text with "Connection issue - retrying...". There's no persistent indicator
of "when was this data last confirmed good," so during a real signal problem
the operator can't tell if they're looking at current or stale data.

**Fix:** Add a small persistent status chip near the top (e.g. next to
`#status`) showing "Live" (green) vs "Last updated Xs ago" (amber) vs
"Offline — retrying" (red), updated on every poll tick regardless of
success/failure, so staleness is always visible even when individual panels
still show old data.

### 1.6 [P1] Touch targets and readability for field/outdoor use
Inputs/buttons already use 12px padding, which is reasonable, but verify all
tap targets meet **44×44px minimum** (WCAG 2.5.5), especially the small
`langBtn`/`debugToggle` buttons and thread-list items. Also:
- Base font size should not go below 16px anywhere (prevents iOS auto-zoom on
  input focus, which is jarring).
- Increase contrast/weight of `.muted` text (`#5f6b7a` on `#f5f7f9`) — check
  it against WCAG AA (4.5:1); it's borderline for outdoor sunlight glare.
  Consider a slightly darker muted tone for anything conveying status
  (RSSI, last-seen, warnings) even if pure decorative captions stay light.

**Fix:** Add explicit `min-height:44px` to `button,input,select,.thread-item`
and bump `.muted` color to something like `#475569` for anything
status-bearing.

### 1.7 [P2] Color-only priority signaling
Priority is shown only as a colored dot/background (`pri-high/normal/low` →
red/amber/green tints). Colorblind users, and anyone glancing quickly, lose
this signal.

**Fix:** Pair every priority indicator with a short text/icon in addition to
color — e.g. a small "‼ HIGH" / "● NORMAL" / "· LOW" badge — not color alone.

### 1.8 [P2] No installable/quick-access affordance
Operators will reopen `http://192.168.4.1` repeatedly during an incident.
There's no favicon or web app manifest, so it looks like a broken/unstyled
page in browser tabs and can't be pinned to a home screen for one-tap access.

**Fix:** Add a minimal inline SVG favicon (data URI, zero extra flash cost)
and a tiny `manifest.json`-equivalent via `<link rel=apple-touch-icon>` /
`<meta name=mobile-web-app-capable content=yes>` so operators can "Add to
Home Screen" and reopen the portal like an app.

---

## 2. Index / Dashboard page (`INDEX_HTML`)

### 2.1 [P0] Critical warnings are visually indistinguishable from footnotes
`#warningBox` (queue-full / duplicate-node-ID warnings) is rendered as a
plain `<p class=muted>` inside the "Portal" card at the **bottom** of the
page — the same visual weight as "Connect to this Wi-Fi when offline...".
These two warnings are operationally important (a full queue silently drops
new messages; a duplicate node ID can corrupt routing/attribution) and are
currently the easiest thing on the page to miss.

**Fix:** Promote to a full-width banner **directly under the header status
bar** (top of page, above "Send Message"), styled like the existing `.warn`
class from `SETUP_HTML` (yellow/amber, bordered, icon), only rendered when a
warning is active. Consider requiring explicit dismissal/acknowledgement for
`queue_full` since it affects whether the next send will succeed.

### 2.2 [P1] Status bar is a dense, hard-to-scan pipe-delimited string
`#status` renders one long line: `Node X | Name | Location | Role | Relay |
AP | Clients | Mailbox | Time` — 9 values crammed into a single sentence
inside the colored header. Under stress this is slow to parse and doesn't
reflow well on narrow screens.

**Fix:** Replace with a small responsive grid of labeled stat "chips" (label
above/beside value), 2 columns on mobile / 4+ on wider screens, each with a
one-word label ("Node", "Role", "Clients", "Mailbox", "Time"...) so the
operator can scan vertically instead of parsing a run-on sentence. Keep it
inside the existing red header bar for visual continuity.

### 2.3 [P1] "Mailbox" and other jargon aren't explained
Terms like "Mailbox", "Relay", raw RSSI/SNR dBm values, and "hop distance"
are shown without explanation, and the intended audience is barangay
officials, not radio engineers.

**Fix:** Add short `title=` tooltips (or a tiny `(?)` inline help icon with a
one-line popover, pure CSS/JS, no library) explaining what each metric means
in plain language, e.g. "Mailbox: messages waiting to send when back in
range." Translate these hints too (see 1.4).

### 2.4 [P1] Signal quality shown only as raw numbers
Mesh Health and Routes panels list raw `RSSI ... dBm | SNR ... dB` and raw
`last_seen_epoch` integers. Nobody in the field reads dBm as "good/bad" at a
glance, and a raw Unix epoch number is meaningless without doing math.

**Fix:**
- Convert `last_seen_epoch` to a relative, human string ("2 min ago", "just
  now", "40 min ago — stale") via a small `timeAgo()` helper, computed
  client-side against `s.epoch`/current time.
- Render RSSI as a simple 1–4 bar signal-strength icon (pure CSS bars,
  colored by threshold) next to the raw dBm value in parentheses for anyone
  who wants the precise number.

### 2.5 [P1] Discover/"Refresh Mesh" gives no in-flight feedback
`discoverBtn` → `refreshDiscovery()` fires a `/discover` request that
involves an actual LoRa round-trip (multi-second, per `DECISIONS.md`'s hop
budget), but the button gives no loading state and can be repeatedly tapped
mid-request, potentially flooding discovery traffic.

**Fix:** On click, disable the button, swap its label to "Discovering… (~4s)"
or show a small inline spinner, and re-enable on response/timeout. Debounce
repeat clicks.

### 2.6 [P1] Messenger (thread list + thread view) doesn't work well as a
mobile inbox
Below 860px the two-column messenger collapses to a single stacked column
(`@media(max-width:860px){.messenger{grid-template-columns:1fr}}`), meaning
on a phone the operator must scroll past the entire thread list to reach the
open conversation, every time a thread is tapped.

**Fix:** On narrow viewports, use a two-view mobile pattern: tapping a thread
hides the thread list and shows only the thread view plus a "‹ Back to
messages" link/button at the top; tapping back restores the list. This is a
CSS class toggle (`.mobile-thread-open`) plus a couple of JS lines — no
framework needed.

### 2.7 [P2] No "new since last visit" indicator
Threads show priority-colored dots but nothing distinguishes messages the
operator hasn't seen yet from ones already reviewed, so on return to the
portal there's no way to tell what's new without re-reading everything.

**Fix:** Track the highest message `id` seen per thread in `localStorage`;
show an unread count/dot on threads with newer messages, clear it when the
thread is opened.

### 2.8 [P2] Message compose form: no character counter, no autosave
`#payload` has `maxlength=159` but no live counter, so users can't tell how
close they are to the cutoff (relevant for a field where every word may
matter, similar to SMS). There's also no draft persistence — if the 15-minute
session idle timeout fires or the page reloads mid-composition, the draft is
lost silently.

**Fix:** Add a small "`n/159`" counter under the textarea, and periodically
save the in-progress form (name/type/priority/destination/payload) to
`localStorage`, restoring it if the page reloads with an empty form.

### 2.9 [P2] Session-expiry cliff
Per `DECISIONS.md`, sessions idle-expire after 15 minutes — but nothing in
the UI warns the user before that happens, so a slow-typing operator can lose
their place mid-task.

**Fix:** Track last-activity client-side; at ~13 minutes idle, show a
dismissible "Your session will expire soon — tap anywhere to stay logged in"
notice.

### 2.10 [P2] Raw packet debug toggle sits at the same visual level as
everyday controls
`debugToggle` ("Show Raw Packet Debug") is a full-width-styled button
directly beside the language toggle — a technical/debug affordance
competing for attention with the two things every operator uses.

**Fix:** Move it into a collapsed "Advanced" disclosure (`<details>` element
— free in plain HTML, no JS needed) at the bottom of the Messages card,
separate from the primary controls.

### 2.11 [P2] OTA upload has no progress indication
`otaBtn` fires a raw `fetch('/ota', {..., body:f})` for a firmware binary
with just a single toast at the very end. A multi-second/minute upload over a
local AP with no progress bar reads as a frozen page.

**Fix:** Switch to `XMLHttpRequest` (needed for `upload.onprogress`, `fetch`
doesn't expose upload progress) and render a simple `<progress>` bar with %
complete during the upload.

---

## 3. Setup Wizard page (`SETUP_HTML`)

### 3.1 [P0] Missing mobile breakpoint — the one page most likely to be filled
out on a phone during initial field deployment
Unlike `INDEX_HTML`, `SETUP_HTML`'s `.grid` (used for every field group) has
**no `@media` rule** to collapse from `1fr 1fr` to a single column on narrow
screens. On a phone this means every setup field is cramped into a ~half-width
box.

**Fix:** Add the equivalent of `@media(max-width:620px){.grid{grid-template-
columns:1fr}}`, matching the pattern already used in `INDEX_HTML`.

### 3.2 [P0] Generated secrets (Web PIN, AP Password, Network Key) aren't
clearly presented as "here's your credential, save it"
Per `FEATURES.md`/`DECISIONS.md`, these are generated with `esp_random()` on
first boot, but the form field just shows placeholder text like "Generated
on first boot" with an empty, required input. It's unclear from the UI
whether the field is pre-filled with the real generated value (and the
placeholder is misleading if so) or genuinely empty (and the operator must
somehow discover the generated value elsewhere, e.g. serial log — not
field-friendly).

**Fix (coordinate with backend behavior):** If these are pre-generated, the
input should show the **actual value**, not a placeholder, styled to make
clear "this was generated for you — write it down before continuing," with a
copy-to-clipboard button next to each field. Add a "Regenerate" button for
users who want a fresh value. This is a life-safety/operational credential —
if the operator can't retrieve it later, they can be locked out of their own
node.

### 3.3 [P0] "Duress PIN" — a safety-critical, easily-misunderstood field —
has almost no explanation
It's a single input with placeholder "Optional silent PIN" and zero
elaboration. If this triggers a silent distress/duress signal (as the name
implies), an operator who doesn't understand it could either never set it (a
missed safety feature) or accidentally treat it as their normal login PIN in
a real emergency, with unclear consequences either way.

**Fix:** Add a short, always-visible explanation (not a tooltip — this one's
too important to hide behind a hover/tap) describing exactly what happens
when the duress PIN is used, directly under the field, in both languages.

### 3.4 [P1] Progress feedback is a plain sentence, not a visual progress
indicator
`#setupWarn` textually lists missing required fields ("Fill: network_key,
web_pin..."), using raw field `name` attributes (not human labels) in the
message. It's functional but low-visibility and shows raw HTML `name=`
tokens instead of the friendly label text ("Network Key" etc.) shown next to
each input.

**Fix:** Map field names to their display labels in `updateWarn()` (small JS
object) so the message reads "Fill: Network Key, Web PIN" instead of raw
attribute names. Optionally pair with a lightweight step-progress bar (e.g.
"3 of 4 required fields set") at the top of the card.

### 3.5 [P1] Factory Reset has equal visual proximity to Save, on a page
where destructive taps are especially costly
The reset form sits directly below the save form in the same card, both
full-width buttons, differing only by button color (`#991b1b` vs `#1f6f5b`).
On a wizard page — where an operator is deployment-focused and moving fast —
this is a likely mis-tap.

**Fix:** Move Factory Reset out of the main flow into its own visually
separated "danger zone" (bordered box, more spacing, smaller/secondary button
style) with the confirm pattern from 1.3.

### 3.6 [P2] No inline validation / no Node ID collision warning before
submit
Per `DECISIONS.md`, Node ID collision is detected during setup, but nothing
in the client-side form warns proactively — the operator finds out only after
submitting.

**Fix (if a check endpoint is feasible):** On blur of the Node ID field,
optionally ping a lightweight check and show inline "This Node ID may already
be in use nearby" before submission, in addition to whatever the backend
already enforces.

---

## 4. Login page (`LOGIN_HTML`)

### 4.1 [P1] No lockout countdown shown to the user
Per `DECISIONS.md`, failed PIN attempts trigger doubling lockout windows
(30s, 60s, 120s...), but the login page has no JS/countdown at all — a
locked-out operator has no way to know how long to wait, and repeated blind
attempts only extend the lockout further.

**Fix:** When the server indicates a lockout is active (via `#loginWarn` or a
new response field), parse the remaining seconds and render a live countdown
("Try again in 0:45") that disables the Unlock button until it reaches zero.

### 4.2 [P2] No PIN-recovery guidance
If an operator forgets the PIN, there's no on-page hint about what to do
(e.g., "Ask your barangay's node administrator" or "A factory reset via the
BOOT button will require re-running setup").

**Fix:** Add a short muted-text hint under the form pointing to the correct
recovery path, matching whatever your actual recovery process is.

### 4.3 [P2] No autofocus, no caps-lock warning
Minor but real friction: the PIN field doesn't receive focus on page load
(operator must tap it first), and there's no caps-lock detection despite the
field likely being case-sensitive.

**Fix:** Add `autofocus` to the PIN input; add a simple `keydown` listener
checking `event.getModifierState('CapsLock')` to show a small "Caps Lock is
on" warning.

---

## 5. Suggested implementation order

1. **P0 items first**, all pages (sections 1.1–1.3, 2.1, 3.1–3.3) — these are
   the ones most likely to cause a lost message, a locked-out node, or an
   accidental wipe during a real incident.
2. **P1 items**, prioritizing the index/dashboard page since it's used
   continuously during an active incident, then setup (used once per node but
   high-stakes), then login.
3. **P2 polish** last, time permitting.

Keep every change inline in the existing C string literals, reuse the
existing CSS variable/class patterns (`.card`, `.muted`, `.warn`, `.row`,
`.grid`) rather than introducing a new styling system, and re-check the
firmware's flash usage after each batch of changes given everything lives in
`src/main.c`'s compiled string constants.






This is where your project starts transitioning from a **good undergraduate thesis** into something approaching a **research-grade mesh protocol**.

I wouldn't implement OLSR, BATMAN, or Babel directly. They're designed for much more capable hardware and can consume significant RAM, flash, and airtime. Instead, I'd keep your protocol and borrow the ideas that make those protocols effective.

---

# 1. Flooding → Controlled Forwarding (Highest ROI)

**Current**

```
A
↓
B
↓
C
↓
D
```

Everyone forwards.

Eventually everyone hears everything.

Simple.

Reliable.

Wasteful.

---

## Better

Each packet carries

```
Packet ID
Origin
TTL
Hop Count
Forwarder
```

Each node already remembers packet IDs.

Now add **forwarding rules**.

Example:

```
If:

Already forwarded
    Drop

TTL == 0
    Drop

RSSI below threshold
    Drop

Better copy already forwarded
    Drop

Otherwise
    Forward
```

Now only useful copies continue.

---

## Even better

Delayed forwarding.

Instead of

```
Receive

Immediately transmit
```

Do

```
Receive

Random delay

If another node already forwarded

Cancel transmission
```

This dramatically reduces duplicate broadcasts.

Many mesh protocols use variations of this idea.

**Difficulty:** ★★☆☆☆

**Benefit:** ★★★★★

---

# 2. Routing → Route Scoring

Current route:

```
Node A

↓

Node B

↓

Node C
```

because

```
3 hops
```

That's okay.

Instead calculate a score.

Example

```
score =

Hop count

+

RSSI penalty

+

Packet loss penalty

+

Congestion penalty

+

Age penalty
```

Example

```
Route A

2 hops

RSSI -118

Loss 35%

Score

62
```

```
Route B

3 hops

RSSI -81

Loss 1%

Score

29
```

Choose

Route B.

Longer.

Much more reliable.

---

## Store route quality

Each route becomes

```
destination

next hop

hop count

RSSI average

SNR average

loss %

last updated

quality score
```

Now routing becomes intelligent.

Difficulty

★★★☆☆

Benefit

★★★★★

---

# 3. Congestion Control

Right now

Imagine

20 nodes

All transmitting

Boom.

Chaos.

---

## Solution 1

Backoff.

Instead of

```
Retry after 200 ms
```

Do

```
Retry after

200 ms

+

Random

0-600 ms
```

Collisions decrease dramatically.

---

## Solution 2

Adaptive retry

If channel busy

Increase delay

```
200 ms

400

800

1600
```

instead of retrying immediately.

---

## Solution 3

Token bucket

Each node only gets

```
5 packets

per second
```

After that

Queue.

No node can monopolize airtime.

---

Difficulty

★★☆☆☆

Benefit

★★★★★

---

# 4. True Priority Scheduling

This is probably the biggest software improvement you could make.

Right now

Queue likely looks like

```
Low

Low

Low

High
```

Transmission order

```
Low

Low

Low

High
```

Not ideal.

---

Instead

Maintain three queues.

```
HIGH

NORMAL

LOW
```

Scheduler

```
while(true)

High?

Transmit

else Normal?

Transmit

else Low?

Transmit
```

Emergency traffic immediately preempts routine traffic.

---

Even better

Weighted scheduling

```
HIGH

HIGH

NORMAL

HIGH

LOW

HIGH

NORMAL
```

so Low never starves forever.

Difficulty

★★☆☆☆

Benefit

★★★★★

---

# 5. Adaptive Link Metrics

This is the biggest "research paper" improvement.

Instead of

```
RSSI

Hop count
```

Maintain statistics.

Example

```
Neighbor

Average RSSI

Average SNR

Packets sent

Packets ACKed

Packet loss

Latency

Retries

Failures

```

Compute

```
Reliability

ACKed / Sent

=

97%
```

Then

```
Quality Score

RSSI

+

Loss

+

Latency

+

Retries
```

Routes automatically improve over time.

Difficulty

★★★★☆

Benefit

★★★★★

---

# 6. Neighbor Health

Every neighbor gets a health score.

Example

```
Neighbor 7

RSSI

-86

Latency

220 ms

Loss

1%

Health

96%
```

If health falls

```
96

↓

82

↓

63

↓

31
```

Automatically search for another route.

No user intervention.

---

# 7. Opportunistic Routing

Suppose

```
A

↓

B

↓

D
```

fails.

Meanwhile

```
A

↓

C

↓

D
```

still exists.

Instead of waiting for timeout

Immediately switch.

Very resilient.

---

# 8. Smarter Discovery

Current

Likely

```
Broadcast

Everyone responds
```

Better

```
Node A

Discovery

↓

Neighbors only

↓

Those neighbors summarize

their neighbors

↓

Done
```

Far less traffic.

---

# 9. Packet Aggregation

Suppose

```
Weather

Battery

Status

Heartbeat
```

Instead of

```
4 LoRa packets
```

Combine into

```
One packet

Weather

Battery

Status

Heartbeat
```

Huge airtime savings.

---

# 10. Duty Cycle Awareness

LoRa is extremely slow.

Don't send

```
Heartbeat

every

5 seconds
```

Instead

Adaptive

```
Network idle

Heartbeat

30 sec

Busy

Heartbeat

120 sec
```

Massive improvement.

---

# What I would prioritize

If I were leading this project, I'd implement these in order:

| Priority | Feature                           | Impact                | Difficulty |
| -------- | --------------------------------- | --------------------- | ---------- |
| ⭐⭐⭐⭐⭐    | Priority queues                   | Very High             | Easy       |
| ⭐⭐⭐⭐⭐    | Route quality scoring             | Very High             | Medium     |
| ⭐⭐⭐⭐⭐    | Randomized forwarding suppression | Very High             | Medium     |
| ⭐⭐⭐⭐☆    | Congestion backoff                | High                  | Easy       |
| ⭐⭐⭐⭐☆    | Link health statistics            | High                  | Medium     |
| ⭐⭐⭐⭐☆    | Adaptive retries                  | High                  | Easy       |
| ⭐⭐⭐☆☆    | Packet aggregation                | Medium                | Medium     |
| ⭐⭐⭐☆☆    | Adaptive heartbeat intervals      | Medium                | Easy       |
| ⭐⭐☆☆☆    | Opportunistic routing             | Medium                | Hard       |
| ⭐⭐☆☆☆    | Full dynamic routing protocol     | Low for your use case | Very Hard  |

## One caution

The biggest mistake I see in thesis projects is chasing advanced algorithms before proving the basics. Your software already has a substantial feature set. A panel is often more impressed by data like:

* 98% packet delivery across 15 nodes
* 180 ms average latency
* Automatic rerouting within 2 seconds after a relay failure
* Stable operation over a 24-hour endurance test

than by saying, "We implemented a BATMAN-inspired routing algorithm."

For your project, I'd spend as much effort measuring and validating the current system as adding new algorithms. If you do decide to enhance the mesh, the four changes that will give you the largest improvement per line of code are:

1. **Priority-aware transmission queues**
2. **Route quality scoring using multiple metrics**
3. **Randomized forwarding suppression to reduce flooding**
4. **Adaptive congestion backoff**

Those four improvements would make the network noticeably more efficient while keeping it lightweight enough for an ESP32 + LoRa platform.
