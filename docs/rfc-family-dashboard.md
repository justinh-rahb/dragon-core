# RFC: Family dashboard — one local pane of glass for all Dragon devices

Status: **📋 Proposed (RFC) — not started.** Design record for discussion; no code yet.

## Motivation

Each Dragon device (DragonBreath, DragonVent, DragonStatus, …) serves its own web UI
on the LAN at its own hostname. To manage a workshop you open `dragonbreath.local`,
then `dragonvent.local`, then the next one — several tabs for one printer's worth of
gear. Vendors (e.g. BIQU) tout a single "one app for all our devices" experience but
haven't shipped it. We can offer a **local-first** version of that by turning the
family of independent web UIs into one pane of glass — reusing the shared SPA every
device already serves, with **no cloud and no extra hardware**.

## Goal

A **Family view**: open any Dragon device and see a live roster of every Dragon device
on the LAN, with at-a-glance state and one-click drill-in to each device's full existing
surface.

- Reuses the shared `dc_ui` SPA — no separate app to build or host.
- Local-first: no cloud, no backend database, no always-on companion box.
- Every product inherits it (the change is in dragon-core).
- Additive and safe: read-only aggregation; controlling a device still uses that
  device's own auth.

## Non-goals

- A native mobile app (see *Alternatives*). Push notifications, background, and
  frictionless discovery are the reasons to go native later — out of scope here.
- Cloud/remote-over-internet access. LAN only.
- Central configuration storage or user accounts.
- Cross-controlling devices from one screen beyond what each device's API already
  allows (the family view aggregates + links; per-device control stays per-device).

## The constraints that shape the design

Three real constraints (storage is **not** one of them — the browser's `localStorage`
is the store, exactly as the SPA already uses it for the control token and theme):

1. **Browsers can't browse mDNS.** JavaScript cannot enumerate `*.local` services, so a
   web page can't discover devices on its own. **But the ESP32s can** (ESP-IDF mDNS
   supports browsing; today they only *advertise* `_http._tcp`). So we push discovery to
   the devices: each device browses for its siblings and exposes the roster over HTTP.
2. **No CORS today.** A page on `dragonbreath.local` fetching `dragonvent.local/api/v2/state`
   is cross-origin and would be blocked. We must add a family-scoped CORS policy.
3. **Devices are `http` only (no TLS).** A page hosted on **`https`** (GitHub Pages, an
   artifact, any public host) cannot fetch `http://` devices — mixed-content is blocked.
   Therefore the family view must be **served by a device** (http origin talking to other
   http origins is fine). It cannot be a webpage hosted off-device, and full PWA/offline
   (service workers need a secure context) is not available over plain http.

## Design

### 1. Device-side discovery (`dc_wifi` / mDNS)
Devices already `mdns_service_add(..., "_http", "_tcp", 80, NULL, 0)`. Add:
- A **family marker** so we find *Dragon* devices, not every `_http._tcp` host (printers,
  NAS, …). Either a dedicated service type `_dragon._tcp`, or TXT records on the existing
  `_http._tcp` advert (`product=dragonbreath`, `api=2`, `ver=…`). *(Leaning a dedicated
  `_dragon._tcp` — cleaner to browse/filter.)*
- **Browsing**: each device periodically browses the family service and caches the
  results `{instance, host, ip, product, version}`. mDNS browse is cheap (no held socket,
  unlike the SSDP Bambu scan), so a light periodic refresh is fine; on-demand-on-request
  is the alternative (mirrors `dc_bambu_discovery`'s on-demand model). See open questions.

### 2. Roster endpoint (HTTP layer)
`GET /api/v2/peers` → the cached family roster, e.g.
```json
{ "self": {"product":"dragonbreath","host":"dragonbreath.local"},
  "peers": [ {"product":"dragonvent","host":"dragonvent.local","version":"v0.5.5"},
             {"product":"dragonstatus","host":"dragonstatus.local","version":"v0.3.0"} ] }
```
Read-only, unauthenticated (it's just presence/identity — no control, no secrets).

### 3. Family-scoped CORS (HTTP layer)
Add `Access-Control-Allow-Origin` so a family page can fetch a sibling's `/api/v2/state`.
Scope is an open question — reflect an `Origin` that is itself a known family host, or a
permissive LAN allowance. Only the **read** surfaces (`/api/v2/state`, `/api/v2/peers`,
info) need cross-origin; **command** endpoints stay control-token-gated regardless of
origin (the token is the real gate; CORS only relaxes the read).

### 4. Family view (`dc_ui` SPA)
A new top-level surface in the shared SPA:
- **Roster** = union of `/api/v2/peers` (auto-discovered) **+** any devices the user added
  by hostname/IP (kept in `localStorage`, since discovery can miss a device on a segmented
  network).
- Each device renders a **live tile** (product, key state — chamber temp / vent state /
  status, online/offline) by cross-fetching its `/api/v2/state`.
- **Drill-in**: clicking a tile opens that device's full existing surface (either navigate
  to `http://<host>/` or load it in-place — the SPA already product-switches from
  `/api/v2/info`).
- **Per-device tokens** live in `localStorage` (same `db_tok` mechanism, keyed per host),
  so control passes through with the right auth; the family view aggregates but never
  bypasses a device's token.

### 5. Storage
`localStorage` only: `{ devices: [ {host, name?, product?, token?} ] }`. Per-browser
(so a phone and a desktop keep their own list) — acceptable for a local tool, and it
avoids any server/database. Auto-discovered peers don't need to be stored; manually-added
ones do.

## Where the code lives
All in **dragon-core**, so every product gets it:
- `dc_wifi` — mDNS TXT/service + periodic browse + peer cache.
- HTTP layer (`dc_portal` / product httpd) — `GET /api/v2/peers` + family CORS.
- `dc_ui` — the Family view surface + `localStorage` roster + cross-fetch tiles.

## Rough effort
- mDNS browse + peer cache + `/api/v2/peers`: ~150–250 lines (`dc_wifi` + a handler).
- CORS: small, but needs a careful scope decision.
- Family view UI + roster/token storage + tiles: moderate `dc_ui` work, reusing the
  existing state-rendering and product-switch code.
- Validation: 2–3 real Dragon devices on one LAN.

## Alternatives considered
- **Native Android app** — the only path that cleanly solves discovery (Android NSD) and
  the http/mixed-content limit, and the only one that can do **push notifications**
  (fault/temp/print-done alerts). Cost: a real, per-platform app to build and maintain.
  Revisit when notifications/discovery-UX justify it; a thin WebView + native-discovery
  wrapper could reuse this same SPA.
- **Home Assistant as the hub** — DragonBreath already does HA MQTT Discovery; if
  vent/status expose themselves, HA becomes the dashboard + mobile app + notifications for
  near-zero new UI. Parallel path for HA users; doesn't help non-HA users.
- **Off-device hosted web app** — blocked by the http/mixed-content constraint (can't be
  served from https and still reach `http://` devices). Rejected.

## Open questions
1. **Service identity** — dedicated `_dragon._tcp` vs TXT records on `_http._tcp`?
2. **Discovery cadence** — light periodic browse with cache, or on-demand per
   `/api/v2/peers` request (à la `dc_bambu_discovery`)? Socket/worker budget matters.
3. **CORS scope** — reflect known-family origins, allow LAN, or `*` on read-only routes
   only? Confirm command routes stay token-gated irrespective of CORS.
4. **Drill-in** — navigate to the device's own origin, or render its surface in-place
   within the family shell (nicer, but cross-origin control needs its token + CORS)?
5. **Roster trust** — anyone on the LAN can advertise `_dragon._tcp`; the roster is just
   presence, and control still needs each device's token, so the risk is cosmetic. Worth
   confirming we're comfortable with that.
6. **Manual add UX** — hostname/IP entry for devices discovery misses (VLANs, mDNS off).

## Out of scope / future
- Native app + push notifications.
- Remote/off-LAN access.
- A shared family presence/config bus beyond mDNS.
