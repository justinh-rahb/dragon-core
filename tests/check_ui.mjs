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
  'id="dc-provisioning"',
  "function dcFetchJson(path,options,retried)",
  "headers['X-DragonBreath-Auth']=tok()",
  "function loadProvisioning()",
  "dcFetchJson('/api/v1/provisioning',{cache:'no-store'})",
  "'/api/v1/provisioning/product'",
  "'/api/v1/system/update'",
]) {
  if (!html.includes(marker)) throw new Error(`missing family-SPA contract marker: ${marker}`);
}

// Provisioning is a full-screen device surface, not a translucent modal over
// live controls. Keep both the backdrop and cards opaque in either theme.
if (!html.includes("background:var(--background);")) {
  throw new Error("provisioning overlay must use the opaque app background");
}
if (!html.includes("background:light-dark(rgb(247 247 247),rgb(38 38 38));")) {
  throw new Error("provisioning cards must use an opaque surface");
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
