# dc_ui

`dc_ui` packages the dependency-free Dragon-family browser SPA as a reproducible
gzip asset. Product firmware keeps ownership of HTTP, OTA, setup, authentication,
and hardware policy; it serves the bytes returned by `dc_ui_spa_asset()`.

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

`capabilities` gates optional screens. An older firmware response without the array
keeps every current screen visible, preserving compatibility with already-shipped
DragonBreath API v2 implementations. Schema `1` is the current family descriptor;
an unknown schema is ignored so the static product identity and complete UI remain
available.

Although route handlers stay product-local, the shared client owns the browser side
of the Dragon API v2 wire contract. A consumer must provide `/api/v2/info`,
`/api/v2/state`, `/api/v2/events`, and the command/settings routes used by the SPA,
or provide a compatible adapter. The client attempts SSE for live updates and falls
back to serialized polling. Multi-device discovery/grouping and any future WebSocket
transport are separate follow-ons.

Consuming builds require a host `gzip`; CMake uses `gzip -9 -n` to generate the
reproducible embedded asset.
