# RFC: Vent ↔ Breath link — DragonBreath as an AUTO information source for DragonVent

Status: **🔨 Accepted — implementing.** Design record; implementation in progress on `feat/vent-breath-link` (dragon-core `dc_breath_link` + `dc_ui`; DragonVent `dv_policy` + `dv_portal`).

---

> ### Implemented as (supersedes the transport below)
>
> The shipped V2 transport is **ESP-NOW push, not HTTP poll**. The `## Design` §1 poll
> task, `<breath-address>` config, JSON parser, and HTTP client below are the **original,
> superseded** design — the implementation uses the `dc_peer` capability plane (RFC 0003/0004):
>
> - **Transport:** the Breath *broadcasts* a heater capability over `dc_peer` (ESP-NOW); the
>   Vent subscribes. Freshness is by local receipt time (same 150 s window). No HTTP, no address.
> - **Same-channel only (known limitation).** ESP-NOW reaches only peers on the current Wi-Fi
>   channel. Devices joined to different mesh nodes/APs on different channels will **not** discover
>   one another. There is no HTTP/mDNS cross-channel fallback; the product promise is narrowed to
>   same-channel discovery. (Cross-channel fallback is possible future work.)
> - **Binding:** the Vent binds to **one specific** Breath (its `peer_id`); there is no
>   "accept any". Discovery is name-clamped to `dragon<kind>-` ids.
> - **Threat model (unauthenticated by design).** `dc_peer` frames are unencrypted broadcast and
>   the `peer_id` is self-reported, so a same-channel sender can spoof a bound id. This is
>   acceptable **only because these frames drive a benign, non-safety actuator** — a vent damper,
>   never a heater. A spoofed frame can at most open or close the damper, both harmless (a closed
>   vent is benign; an open vent is normal cooldown). Per `dc_peer`'s rule, these frames carry
>   meaning, not safety authority. Pair-time encryption (LMK) is deferred; any consumer that would
>   drive a *safety-relevant* output from these frames MUST add authentication first.

## Motivation

DragonVent's AUTO mode opens/closes the vent from the printer's state: it seals for a
heat-retaining filament (ASA/ABS/PC/PA) during a print and opens otherwise. Two real
cases it cannot handle today:

- **Heat soak.** Before a print, the chamber is brought up to temperature. The vent
  should be **sealed** to let it heat, but nothing on the *printer* says "a chamber
  heat-soak is in progress." (A shipped V1 link infers this from the DragonBreath
  device *only if* the `dragonbreath-klipper` helper is installed on the same
  Moonraker — see Background — but that is Klipper-only and coarse.)
- **Cooldown / print-over.** When a print finishes, the vent should **open** to vent
  the chamber. Today AUTO keys the idle decision off **bed temperature**
  (`bed_temp > 45 °C → stay closed`, `dv_policy.c:179-181`). But a just-finished ABS
  print leaves the bed at 90–100 °C for many minutes, so bed temp reads "still hot,
  keep sealed" and cannot distinguish a finished print from an active one. The
  last-known filament name persists too. Neither signal has an *edge* at "print
  complete," so the vent stays sealed long past when it should open.

The fix has two halves: **use the printer's own print-state** (which flips to
idle/complete instantly) instead of bed temperature to detect print-over, and
**consume the DragonBreath's heater state directly** so heat-soak, chamber-hold, and
filament-dry seal the vent regardless of control source.

## Background — what exists today

- **AUTO decision:** `decide_auto_target()` (`DragonVent/firmware/components/dv_policy/dv_policy.c:157`),
  called at 1 Hz from `policy_task` (`dv_policy.c:184`). Inputs in `auto_input_t`
  (`dv_policy.c:85-93`): `reliable`, `error`, `active`, `chamber_heating`, `bed_temp`,
  `material`, `state`. Filament rules: `material_preference()` (`dv_policy.c:60-74`).
- **Printer state:** the Vent subscribes to Moonraker directly via shared
  `dc_moonraker`; `dv_policy` reads `dc_moonraker_get_status()` (`dv_policy.c:102`).
  Print-state is a six-state enum (`dc_moonraker.h:22-30`), surfaced as `active`.
- **Shipped V1 Breath link (Moonraker-mediated):** the `dragonbreath-klipper` helper
  republishes the Breath's `/api/v2/state` as a Klipper object `dragonbreath`
  (`dragonbreath.py:897-925`); the Vent parses six `db_*` fields
  (`dc_moonraker.c:299-314`) into a single `chamber_heating` boolean
  (`dv_policy.c:116-119`) that forces CLOSED during a heat soak. This RFC supersedes
  that coarse signal with a direct, source-agnostic link.

## Goal

Make DragonVent's AUTO decision fuse **two** signals:

1. **Printer state + filament** from Moonraker (already available) — drives the
   print/cooldown behavior off the **print-state edge**, not bed temperature.
2. **DragonBreath heater state** from a direct, source-agnostic HTTP poll (new) —
   seals the vent whenever the Breath is actively heating (soak, hold, or drying).

The Breath is an **additional information source, never a control source**: opt-in per
Vent, advisory only, and the Vent falls back to printer-only behavior whenever it is
absent, unreachable, or stale.

## Non-goals

- Making the Breath a `dc_source` control source. Control-source arbitration is
  untouched.
- Bidirectional control (the Breath does not obey the Vent).
- Auto-discovery of the Breath (mDNS). The Vent is pointed at a configured address;
  LAN discovery is a future nicety (see the Family-dashboard RFC).
- Any dependency on the `dragonbreath-klipper` Moonraker helper. The direct link works
  whether or not Klipper is in the picture.

## The constraints that shape the design

- **Single-core Vent.** DragonVent runs on a classic ESP32; a busy task starves
  WiFi/httpd (the failure mode we hit on the wheeze). Network work must be a
  **dedicated low-priority task on a gentle cadence**, never on the policy or httpd
  task.
- **The Breath's httpd is small.** Single worker, ~7-socket pool. One light client
  polling infrequently is negligible; anything chatty is not.
- **Advisory, fail-safe.** The Breath signal can vanish at any time (device off, LAN
  blip). AUTO must never wedge or mis-seal when it does — it degrades to printer-only.
- **Slow-moving signal.** Heat soaks and cooldowns run for many minutes, so a coarse
  (≥ 60 s) view of the Breath heater state is entirely sufficient.

## Design

### 1. Transport — direct HTTP poll (source-agnostic)

The Vent polls the Breath's existing read-only state endpoint:

`GET http://<breath-address>/api/v2/state` — **no auth** (state read is open; only
writes/OTA need the control token).

- **Cadence:** every **60 s**, and **only while the Vent is in AUTO mode** with a
  Breath configured. ~1 request/min from one client.
- **Timeout:** ~2–3 s connect+read. A slow/failed poll is a non-event (see staleness).
- **Parse:** targeted scan for the heater subset only, not a full walk of the ~KB
  state (fields serialized by the Breath at `DragonBreath/components/pb_httpd/pb_httpd.c:123-235`):
  - `mode` (off / power_on / auto / drying)
  - `target.effective_c` (fallback `requested_c`)
  - `heater.demand`
  - `safety.fault_latched`, `safety.inhibited`
  - `sensors.chamber.temperature_c`, `sensors.chamber.status` (display)
  - `state_revision` (cheap change detection)

### 2. Snapshot + staleness (non-blocking)

The poll task writes a small snapshot behind a mutex; the policy task reads the
**last-known** snapshot and never touches the network.

```c
typedef struct {
    bool     valid;              // a good poll has ever landed
    int64_t  updated_us;         // monotonic time of last good poll
    bool     connected;          // breath reachable on the last poll
    char     mode[12];           // off / power_on / auto / drying
    float    target_c;
    float    chamber_c;
    bool     demand, fault, inhibited;
    uint32_t state_revision;
} dc_breath_snapshot_t;
```

- **Fresh** = `valid && (now - updated_us) < 150 s` (≈ 2–3 missed polls) — a single
  dropped request does not flip the Breath to "unreachable."
- **Stale / never-seen** → the policy treats the Breath as **no-signal** and decides
  on printer state alone.

### 3. "Heater running" definition (the rule-2 signal)

The Breath is *actively heating* when — from a **fresh** snapshot —

```
connected && !fault && !inhibited && target_c > 0
    && mode ∈ { power_on, auto, drying }
```

This keys off the **stable heating job** (mode + target), not the instantaneous SSR
output — which time-proportions on/off every ~10 s and would otherwise flap the vent
motor. It stays true through a soak/hold/dry and goes false the moment the Breath
returns to `off` / target 0, which is the "heater off → open" trigger.

### 4. AUTO decision

`decide_auto_target()` becomes (highest priority first):

| # | Condition | Vent |
|---|---|---|
| 1 | printer unreliable / ERROR | hold current |
| 2 | **Breath configured** AND heater running (§3) | **CLOSED** — heat soak / chamber hold / filament dry |
| 3 | `active` (printing/preparing/paused) AND seal-filament (ASA/ABS/PC/PA) | **CLOSED** — sealing print |
| 4 | `!active` AND heater not running | **OPEN** — print over + heater off → cooldown |
| 5 | `active` AND non-seal filament | **OPEN** — e.g. PLA |

- `active` and the filament come from Moonraker (`dc_moonraker`), unchanged.
- **Bed temperature is removed from the decision** — the print-state edge (`active`)
  is the reliable "print over" signal; bed temp stays for display/telemetry only.
- When **no Breath is configured**, rule 2 never fires and the poll task never runs;
  the Vent decides on rules 1/3/4/5 alone.

Worked example (matches an observed live state): printer **idle**, bed **45.0 °C**,
material **ASA**, no active Breath job → `!active` + heater not running → **rule 4 →
OPEN**. Under the old bed-threshold logic, `bed_temp == 45` is the ambiguous boundary;
the print-state edge decides it outright.

### 5. Configuration + UI

**Settings** gains a *DragonBreath* section (persisted in NVS via `dv_portal`):

- `Enable DragonBreath info source` — toggle, default **off**
- `Address` — `dragonbreath.local` or an IP

An enabled address is the **"Breath configured"** signal — explicit, no discovery
required.

**Status card** — a block under the existing *Printer & controller* section, shown
only when configured:

```
DragonBreath  (info source)
  Link          connected            ← fresh (< 150 s) else "unreachable"
  Heater        running · auto        ← the rule-2 signal, at a glance
  Chamber       52 → 60 °C            ← chamber_c → target_c (display only)
```

### 6. Where the code lives

- **`dc_breath_link`** — new shared component in dragon-core: the poll task, the
  snapshot struct + mutexed accessor, and config load/save. Reusable (a Breath could
  later watch a Vent, etc.).
- **`dv_policy`** — reads `dc_breath_link_get()` for rule 2; the rest of the table is
  local logic. `auto_input_t` gains `breath_configured` + `breath_heating`;
  `read_auto_input()` fills them; the bed-temp branch is deleted.
- **`dv_portal` + `dc_ui`** — the settings fields and the card block.

## Failure modes & safety

- **Breath off / unreachable / LAN blip** → snapshot goes stale → rule 2 falls out →
  printer-only AUTO. The vent never wedges; worst case a heat soak is not sealed
  (recoverable, and only when the configured Breath is down).
- **Breath faulted / inhibited** → the `!fault && !inhibited` guard drops rule 2 (a
  tripped Breath is not heating).
- **Never authoritative** — the Breath can only ever *seal* via rule 2; it cannot
  force the vent open or override the printer-state cooldown. Advisory input, not
  control.
- **Single-core discipline** — all network work is on one low-priority task at 60 s;
  the policy loop and httpd never block on the Breath.

## Rough effort

- `dc_breath_link` (poll task + snapshot + config): ~1 day.
- `dv_policy` rewrite to the table + Breath input: ~½ day (it *removes* the bed-temp
  branch).
- Settings + card UI: ~½ day.
- HIL / bench validation (heat-soak seal, print-over open, Breath-offline fallback):
  ~1 day.

## Alternatives considered

- **Extend the shipped V1 Moonraker `db_*` path** — enrich the fields the helper
  already republishes (`dc_moonraker.c:299-314`). Lower effort, but Klipper-only and
  requires the `dragonbreath-klipper` helper on a shared Moonraker — not
  source-agnostic. Kept as an optional fallback signal where it exists; the direct
  link is the primary design.
- **mDNS auto-discovery** of the Breath instead of a configured address. Deferred to
  the Family-dashboard RFC's `/api/v2/peers` mechanism (unbuilt); a typed address is
  simpler and sufficient here.
- **A minimal `/api/v1/heat-status` endpoint** on the Breath returning just the heater
  subset. Marginally lighter than `/api/v2/state`, but adds Breath-side surface for
  little gain at a 60 s cadence. Revisit only if payload size becomes a concern.

## Open questions

- Poll only in AUTO, or always-when-configured so the card shows live Breath status in
  MANUAL too? (Leaning AUTO-only to honor "light touch"; the card shows "—" in MANUAL.)
- The 60 s cadence and 150 s fresh-window are starting values — confirm against real
  heat-soak / cooldown timing on hardware.
- Are there Breath `mode` values beyond `power_on` / `auto` / `drying` that should also
  seal (e.g. a future "preheat")?

## Out of scope / future

- mDNS auto-discovery + a peer roster (Family-dashboard RFC).
- Vent → Breath signalling (a Breath reacting to the Vent).
- Multiple Breaths per Vent.
