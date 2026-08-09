# dc_portal

`dc_portal` is the common Dragon-family management plane. It starts the ESP-IDF
HTTP server, serves the `dc_ui` SPA in both station and AP modes, runs captive DNS,
and exposes JSON endpoints for Wi-Fi scans and credentials, fallback-AP settings,
event logs, OTA, and factory reset.

Products supply identity, their API route table, and optional callbacks:

- `describe_product` returns a schema containing sections and fields for settings
  that are not common to every Dragon device.
- `apply_product` validates and persists submitted product settings. Existing NVS
  namespaces and keys remain the product/component's responsibility.
- `authorize` gates mutations. Wi-Fi provisioning remains open only while the
  device is in captive-portal mode so an unconfigured device is recoverable.
- `guard_operation` keeps product safety policy in force before OTA or reset.
- `validate_image` restricts which ESP-IDF project identities may be selected
  for the next boot after an otherwise valid OTA upload.
- `factory_reset` clears product settings before core clears Wi-Fi credentials.
- `httpd_config` and `register_product_routes` preserve product server tuning and
  allow an existing API implementation to register routes without owning the
  server lifecycle.

The provisioning contract is versioned separately at
`GET /api/v1/provisioning`. Product API v2 state and command semantics are not
interpreted by core; the supplied route table continues to own them.
