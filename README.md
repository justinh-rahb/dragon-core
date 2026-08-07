# dragon-core

Shared, board-neutral ESP-IDF components for the Dragon firmware family.
Product repositories own GPIO mappings, sensors, actuators, safety policy, and their
`dc_device` implementation. This repository owns reusable transport and service code.

## Initial component set

| Component | Responsibility |
|---|---|
| `dc_evlog` | In-memory event and diagnostic-console rings |
| `dc_source` | Persisted external-control source selection |
| `dc_bambu` | Bambu LAN MQTT client and printer status |
| `dc_wifi` | Station/AP networking, provisioning state, scanning, and mDNS |
| `dc_moonraker` | Moonraker WebSocket client and Klipper print status |

The first extraction deliberately starts with components already proven in
DragonBreath and free of board/sensor/actuator dependencies. HTTP, OTA, the portal,
the shared SPA, and the `dc_device` capability boundary follow after their current
DragonBreath hardware coupling is removed behind golden API-response tests.

## Consuming from an ESP-IDF application

Add the components used by the application to its component manifest. Pin every
component to the same tag or full commit SHA:

```yaml
dependencies:
  dc_evlog:
    git: https://github.com/justinh-rahb/dragon-core.git
    path: components/dc_evlog
    version: <tag-or-commit>
  dc_source:
    git: https://github.com/justinh-rahb/dragon-core.git
    path: components/dc_source
    version: <tag-or-commit>
  dc_bambu:
    git: https://github.com/justinh-rahb/dragon-core.git
    path: components/dc_bambu
    version: <tag-or-commit>
  dc_wifi:
    git: https://github.com/justinh-rahb/dragon-core.git
    path: components/dc_wifi
    version: <tag-or-commit>
  dc_moonraker:
    git: https://github.com/justinh-rahb/dragon-core.git
    path: components/dc_moonraker
    version: <tag-or-commit>
```

Applications should commit `dependencies.lock` when consuming tagged or commit-pinned
revisions. Listing the components directly is intentional: the Component Manager
bundled with ESP-IDF 5.3 downloads only the selected Git subdirectory, so an aggregate
component cannot resolve sibling directories with `override_path`.

For local development in a sibling checkout, use a path dependency temporarily:

```yaml
dependencies:
  dc_evlog:
    path: ../../dragon-core/components/dc_evlog
  dc_source:
    path: ../../dragon-core/components/dc_source
  dc_bambu:
    path: ../../dragon-core/components/dc_bambu
  dc_wifi:
    path: ../../dragon-core/components/dc_wifi
  dc_moonraker:
    path: ../../dragon-core/components/dc_moonraker
```

## Compatibility

The public C prefix is `dc_`. The extraction intentionally preserves existing NVS
namespaces and keys so moving DragonBreath from its former local `pb_*` copies does not
erase saved Wi-Fi, Moonraker, Bambu, or control-source configuration.

Requires ESP-IDF 5.3 or newer.

## License

MIT. See [LICENSE](LICENSE).
