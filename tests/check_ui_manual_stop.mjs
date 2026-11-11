import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const html = fs.readFileSync(new URL("../components/dc_ui/www/app.html", import.meta.url), "utf8");

function section(start, end) {
  const a = html.indexOf(start);
  const b = html.indexOf(end, a);
  assert.ok(a !== -1 && b > a, `missing UI section: ${start}`);
  return html.slice(a, b);
}

// Execute the real helper, renderer and event handlers in a tiny DOM/command
// harness. No browser, network, product firmware or heater is involved.
const code = section("  function manualActionForState(s){", "  function renderAuto(s){")
  + "\n" + section("  var manualIntent = null;", "  $('a-action').addEventListener('click'");
const listeners = {};
const sent = [];
const action = { label: "", icon: "", danger: false };
const statuses = [];
const context = {
  last: null, lease: null, fields: { man: { val: 50 } }, touched: { man: 1 },
  $: (id) => {
    assert.equal(id, "m-action");
    return { addEventListener: (name, fn) => { listeners[name] = fn; } };
  },
  setAction: (id, label, icon, danger) => { assert.equal(id, "m-action"); Object.assign(action, { label, icon, danger }); },
  setStatus: (id, text) => { assert.equal(id, "m-status"); statuses.push(text); },
  command: (name, args) => { sent.push({ name, args }); return Promise.resolve({ lease_id: "test" }); },
  runCmd: (_id, promise) => promise,
};
vm.runInNewContext(code, context, { filename: "dc_ui-manual-handler.js" });

const ready = () => ({ mode: "off", target: { effective_c: 0 }, safety: { inhibited: false, fault_latched: false } });
const heating = () => ({ mode: "power_on", target: { effective_c: 50 }, safety: { inhibited: false, fault_latched: false } });
const fault = () => ({ mode: "off", target: { effective_c: 0 }, safety: { inhibited: false, fault_latched: true } });
const inhibited = () => ({ mode: "off", target: { effective_c: 0 }, safety: { inhibited: true, fault_latched: false } });

async function check(label, initial, beforeClick, expected, event = "pointerdown") {
  sent.length = 0;
  context.last = initial;
  if (event) listeners[event](event === "keydown" ? { key: "Enter" } : {});
  if (beforeClick) context.last = beforeClick;
  listeners.click();
  await Promise.resolve();
  assert.deepEqual(sent.map((x) => x.name), expected, label);
}

// Normal operations.
await check("start while ready", ready(), null, ["power_on"]);
await check("stop while heating", heating(), null, ["off"]);
// Faults and asynchronous state changes must never convert Stop into Start.
await check("fault arrives during Stop gesture", heating(), fault(), ["off"]);
await check("mode turns off during Stop gesture", heating(), ready(), ["off"]);
await check("fault arrives during Start gesture", ready(), fault(), ["off"]);
await check("faulted state without pointer event", fault(), null, ["off"], null);
await check("inhibited state without pointer event", inhibited(), null, ["off"], null);
await check("keyboard Stop survives mode update", heating(), ready(), ["off"], "keydown");
// No state must never submit a start request.
await check("no device snapshot", null, null, [], null);

// An abandoned pointer gesture should not force a future interaction to Stop.
sent.length = 0;
context.last = heating();
listeners.pointerdown();
listeners.pointercancel();
context.last = ready();
listeners.click();
assert.deepEqual(sent.map((x) => x.name), ["power_on"]);

// Fault presentation stays visibly safe rather than silently relabeling as Start.
context.last = fault();
context.renderManual(context.last);
assert.equal(action.label, "Ensure heater off");
assert.match(statuses.at(-1), /Heater off.*fault latched/);
context.last = inhibited();
context.renderManual(context.last);
assert.equal(action.label, "Ensure heater off");
assert.match(statuses.at(-1), /Heating unavailable/);
context.last = ready();
context.renderManual(context.last);
assert.equal(action.label, "Start heating");

console.log("dc_ui manual Stop/fault interaction: PASS");
