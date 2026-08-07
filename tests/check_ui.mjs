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
]) {
  if (!html.includes(marker)) throw new Error(`missing family-SPA contract marker: ${marker}`);
}

console.log("family SPA JavaScript and capability contract: PASS");
