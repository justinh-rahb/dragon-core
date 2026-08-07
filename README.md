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

Add the aggregate component to the application's component manifest:

```yaml
dependencies:
  dragon_core:
    git: https://github.com/justinh-rahb/dragon-core.git
    path: components/dragon_core
    version: <tag-or-commit>
```

The aggregate manifest resolves the sibling `dc_*` components from the same revision.
Applications should commit `dependencies.lock` when consuming tagged or commit-pinned
revisions.

For local development in a sibling checkout, use a path dependency temporarily:

```yaml
dependencies:
  dragon_core:
    path: ../../dragon-core/components/dragon_core
```

## Compatibility

The public C prefix is `dc_`. The extraction intentionally preserves existing NVS
namespaces and keys so moving DragonBreath from its former local `pb_*` copies does not
erase saved Wi-Fi, Moonraker, Bambu, or control-source configuration.

Requires ESP-IDF 5.3 or newer.

## License

MIT. See [LICENSE](LICENSE).
