# dc_ui

`dc_ui` packages the dependency-free Dragon-family browser SPA as a reproducible
gzip asset. `dc_portal` serves it and owns the shared provisioning/recovery
transport. Product firmware keeps ownership of authentication, API v2, and
hardware policy.

The SPA discovers product identity and supported surfaces from `GET /api/v2/info`.
The additive family descriptor is:

```json
{
  "capabilities": ["power_on", "auto", "drying"],
  "ui": {
    "schema": 1,
    "product": "dragonbreath",
    "display_name": "DragonBreath"
  }
}
```

DragonVent selects its dedicated airflow surface with an additive descriptor:

```json
{
  "capabilities": ["vent_manual", "vent_auto", "source_status", "polling"],
  "ui": {
    "schema": 1,
    "product": "dragonvent",
    "display_name": "DragonVent"
  }
}
```

That surface uses the same responsive shell and appearance controls as
DragonBreath, but has vent-specific state, manual open/close controls, and the
automatic bed-temperature policy. It does not reinterpret vent motion as heater
state. DragonVent consumers provide `vent`, `printer`, `policy`, and `wifi` objects
in `/api/v2/state`, plus the compact `/api/v2/command` and `/api/v2/settings`
adapters documented by their product firmware.

`capabilities` gates optional screens. An older firmware response without the array
keeps every current screen visible, preserving compatibility with already-shipped
DragonBreath API v2 implementations. Schema `1` is the current family descriptor;
an unknown schema is ignored so the static product identity and complete UI remain
available.

Although route handlers stay product-local, the shared client owns the browser side
of the Dragon API v2 wire contract. A consumer must provide `/api/v2/info`,
`/api/v2/state` and the command/settings routes used by its selected surface. SSE at
`/api/v2/events` is optional: the client attempts it first and falls back to
serialized polling when it is unavailable. Multi-device discovery/grouping and any
future WebSocket transport are separate follow-ons.

Consuming builds require a host `gzip`; CMake uses `gzip -9 -n` to generate the
reproducible embedded asset.

The SPA renders `dc_portal`'s versioned `/api/v1/provisioning` schema in a common
setup overlay. It opens automatically in AP mode, so the same SPA is the normal UI
on the LAN and on the captive setup network. Product-specific fields are described
by firmware callbacks rather than compiled into another server-rendered page.
