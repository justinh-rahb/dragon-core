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
| `dc_mqtt` | Shared ESP-MQTT session lifecycle, LWT, and event callbacks |
| `dc_ui` | Embedded, capability-aware browser SPA shared by Dragon products |
| `dc_portal` | Shared HTTP server, provisioning API, captive DNS, OTA, logs, and recovery routes |

The initial service extraction deliberately started with components already proven in
DragonBreath and free of board/sensor/actuator dependencies. `dc_ui` owns the static
family SPA; `dc_portal` owns its HTTP delivery and the board-neutral provisioning and
recovery plane. Products register API routes and provide a schema plus callbacks for
their own settings, reset behavior, and optional authorization.

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
  dc_mqtt:
    git: https://github.com/justinh-rahb/dragon-core.git
    path: components/dc_mqtt
    version: <tag-or-commit>
  dc_ui:
    git: https://github.com/justinh-rahb/dragon-core.git
    path: components/dc_ui
    version: <tag-or-commit>
  dc_portal:
    git: https://github.com/justinh-rahb/dragon-core.git
    path: components/dc_portal
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
  dc_mqtt:
    path: ../../dragon-core/components/dc_mqtt
  dc_ui:
    path: ../../dragon-core/components/dc_ui
  dc_portal:
    path: ../../dragon-core/components/dc_portal
```

## Compatibility

The public C prefix is `dc_`. The extraction intentionally preserves existing NVS
namespaces and keys so moving DragonBreath from its former local `pb_*` copies does not
erase saved Wi-Fi, Moonraker, Bambu, or control-source configuration.

Requires ESP-IDF 5.3 or newer.

For local and CI builds, use `tools/idf-build.sh <project> <target> <build-dir>`.
It explicitly locates the Xtensa/RISC-V compiler required by the target and rejects
stale Component Manager locks before CMake can quietly reuse a different pinned
core revision.

## License

MIT. See [LICENSE](LICENSE).
