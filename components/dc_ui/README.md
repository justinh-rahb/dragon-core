# dc_ui

`dc_ui` packages the dependency-free Dragon-family browser SPA as a reproducible
gzip asset. Product firmware keeps ownership of HTTP, OTA, setup, authentication,
and hardware policy; it serves the bytes returned by `dc_ui_spa_asset()`.

The SPA discovers product identity and supported surfaces from `GET /api/v2/info`.
The additive family descriptor is:

```json
{
  "capabilities": ["power_on", "auto", "drying", "sse"],
  "ui": {
    "schema": 1,
    "product": "dragonbreath",
    "display_name": "DragonBreath"
  }
}
```

`capabilities` gates optional screens. An older firmware response without the array
keeps every current screen visible, preserving compatibility with already-shipped
DragonBreath API v2 implementations. Product-specific state and commands remain on
their existing routes; this extraction does not change their wire contracts.

The current live transport is HTTP plus SSE (`/api/v2/state` and
`/api/v2/events`). Multi-device discovery/grouping and any future WebSocket transport
are separate follow-ons.
