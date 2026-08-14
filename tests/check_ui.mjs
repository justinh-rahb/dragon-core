import fs from "node:fs";

// The family dashboard/control UI is an embedded SPA, not a C string.
const html = fs.readFileSync(
  new URL("../components/dc_ui/www/app.html", import.meta.url),
  "utf8",
);

const scripts = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)].map(
  (m) => m[1],
);
if (!scripts.length) throw new Error("dashboard script not found");

// Syntax-check each inline script (compile only; never executed). The token
// helpers are runtime dependencies, not syntax dependencies.
for (const script of scripts) {
  new Function(script);
}

// Guard against duplicate element ids in the served markup.
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]);
const seen = new Set();
const dup = ids.find((id) => seen.has(id) || (seen.add(id), false));
if (dup) throw new Error(`duplicate element id: ${dup}`);

for (const marker of [
  'data-capability="power_on"',
  'data-capability="auto"',
  'data-capability="drying"',
  "function applyDeviceInfo(i)",
  "if(ui.schema!=null && ui.schema!==1) return",
  "(title&&title.textContent)||document.title",
  'ui.product===\'dragonvent\'',
  'id="dv-app"',
  'data-vent-content="manual"',
  'data-vent-content="auto"',
  "function applyVent(s)",
  "ventPost('/api/v2/settings'",
  "ventCommand('manual'",
  // DragonWheeze (Sovol SH01 filament dryer) surface.
  'ui.product===\'dragonwheeze\'',
  'id="dw-app"',
  'data-dry-content="presets"',
  'data-dry-content="manual"',
  "function applyDry(s)",
  "dryCommand('set_preset'",
  "dryCommand('set_target'",
  // The sensor fault must stay a first-class element of the dryer dashboard: it
  // is the only signal that the ambient pair has stopped being trustworthy.
  'id="dw-fault"',
  'id="dc-provisioning"',
  "function dcFetchJson(path,options,retried)",
  // Auth transport: exactly one token derivation, used by dcFetchJson, and
  // sending both the legacy and the family-neutral header name.
  "function dcTok()",
  "headers['X-DragonBreath-Auth']=t;",
  "headers['X-Dragon-Auth']=t;",
  "var headers=dcAuthHeaders(",
  "function loadProvisioning()",
  "dcFetchJson('/api/v1/provisioning',{cache:'no-store'})",
  "'/api/v1/provisioning/product'",
  "'/api/v1/system/update'",
]) {
  if (!html.includes(marker)) throw new Error(`missing family-SPA contract marker: ${marker}`);
}

// The Sovol SH01 front panel offers exactly three temperatures (40/45/50 °C), so
// the dryer's manual target is a selection and not a range. A free-running slider
// here would let the UI request a setpoint the hardware can never reach, and the
// firmware would tap M/A forever trying to land on it.
const dryTemps = [...html.matchAll(/data-dry-temp="(\d+)"/g)].map((m) => m[1]);
if (dryTemps.join(",") !== "40,45,50") {
  throw new Error(`DragonWheeze must offer exactly 40,45,50 °C; found: ${dryTemps.join(",") || "none"}`);
}

// Regression guard for #14: the auth transport must stay product-agnostic. It was
// previously attached only when product === 'dragonbreath' (and the 403 recovery
// was gated the same way), which left every other product with no authenticated
// transport and no way to adopt a control token. Assert the gate cannot return,
// and that the token is derived in exactly one place.
for (const forbidden of [
  "product==='dragonbreath' && typeof tok",
  "r.status===403 && product===",
]) {
  if (html.includes(forbidden)) {
    throw new Error(`auth transport must not be gated on product identity: ${forbidden}`);
  }
}
const derivations = html.split("localStorage.getItem('db_tok')").length - 1;
if (derivations !== 1) {
  throw new Error(`expected exactly one control-token derivation, found ${derivations}`);
}

// dc_portal's /console is a standalone document, so it cannot import the SPA's
// helper and keeps its own copy. That agreement is convention, not code — assert
// it, or a future edit could silently diverge (wrong storage key, or dropping a
// header name) and fail to authorize against a product reading only one of them.
const portal = fs.readFileSync(
  new URL("../components/dc_portal/dc_portal.c", import.meta.url),
  "utf8",
);
for (const [what, needle] of [
  ["the same localStorage key as the SPA", "localStorage.getItem('db_tok')"],
  ["the legacy header name", "'X-DragonBreath-Auth':t"],
  ["the family-neutral header name", "'X-Dragon-Auth':t"],
]) {
  if (!portal.includes(needle)) {
    throw new Error(`/console page must use ${what}: ${needle}`);
  }
}

// Provisioning is a full-screen device surface, not a translucent modal over
// live controls. Keep both the backdrop and cards opaque in either theme.
if (!html.includes("background:var(--background);")) {
  throw new Error("provisioning overlay must use the opaque app background");
}
if (!html.includes("background:light-dark(rgb(247 247 247),rgb(38 38 38));")) {
  throw new Error("provisioning cards must use an opaque surface");
}
if (!html.includes("#dc-product-setup{ display:flex; flex-direction:column; gap:10px; }")) {
  throw new Error("dynamic provisioning cards must retain the shell spacing");
}

// Filament zones only take effect in AUTO during an active print. The zones card is
// the only place this is stated now that product setup pages no longer edit zones.
if (!/Zones do nothing in Manual\/Off\./.test(html)) {
  throw new Error("filament zones card must state that zones apply only in AUTO");
}

// Common maintenance identity must render before descriptor compatibility or
// capability gating can return/throw. This keeps diagnostics useful even when a
// newer product descriptor reaches an older family SPA.
const infoStart = html.indexOf("function applyDeviceInfo(i)");
const firmwareRender = html.indexOf("if(i.firmware) u('s-fw', i.firmware);", infoStart);
const schemaGate = html.indexOf("if(ui.schema!=null && ui.schema!==1) return", infoStart);
if (infoStart < 0 || firmwareRender < infoStart || schemaGate < firmwareRender) {
  throw new Error("maintenance identity must render before descriptor gating");
}

console.log("family SPA JavaScript and capability contract: PASS");
