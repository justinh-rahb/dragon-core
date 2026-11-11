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

// Run the actual shared UI renderer and click handlers against mock DOM and
// commands, without networking or a heater. The extraction deliberately checks
// known section boundaries so changes to the surrounding SPA are noticed.
const code = section("  function manualStartAllowed(s){", "  function renderAuto(s){")
  + "\n" + section("  $('m-stop').addEventListener('click'", "  $('a-action').addEventListener('click'");
const sent = [];
const buttons = {
  "m-start": { disabled: true, listeners: {} },
  "m-stop": { disabled: false, listeners: {} },
};
const statuses = [];
const context = {
  last: null, lease: null, fields: { man: { val: 50 } }, touched: { man: 1 },
  $: (id) => {
    assert.ok(buttons[id], `unexpected button ${id}`);
    return {
      get disabled() { return buttons[id].disabled; },
      set disabled(value) { buttons[id].disabled = value; },
      addEventListener: (name, fn) => { buttons[id].listeners[name] = fn; },
    };
  },
  setStatus: (id, message) => { assert.equal(id, "m-status"); statuses.push(message); },
  command: (name, args) => { sent.push({ name, args }); return Promise.resolve({ lease_id: "test" }); },
  runCmd: (_id, promise) => promise,
};
vm.runInNewContext(code, context, { filename: "dc_ui-manual-handler.js" });

// The two controls must never exchange IDs or labels during an SSE render.
assert.match(html, /id="m-start"[^>]*>[^]*?<span>Start heating<\/span>/);
assert.match(html, /id="m-stop"[^>]*>[^]*?<span>Stop<\/span>/);
assert.ok(!html.includes('id="m-action"'), "old toggle must be removed");
assert.ok(!html.includes("manualIntent"), "gesture intent bookkeeping must be removed");

const ready = () => ({ mode: "off", target: { effective_c: 0 }, safety: { inhibited: false, fault_latched: false } });
const heating = () => ({ mode: "power_on", target: { effective_c: 50 }, safety: { inhibited: false, fault_latched: false } });
const fault = () => ({ mode: "off", target: { effective_c: 0 }, safety: { inhibited: false, fault_latched: true } });
const inhibited = () => ({ mode: "off", target: { effective_c: 0 }, safety: { inhibited: true, fault_latched: false } });
const otherMode = (mode) => ({ mode, target: { effective_c: 50 }, safety: { inhibited: false, fault_latched: false } });

function paint(s) {
  context.last = s;
  if (s) context.renderManual(s);
}
function click(id) {
  // Simulate ordinary user activation (disabled controls cannot be clicked).
  if (buttons[id].disabled) return;
  buttons[id].listeners.click();
}
function assertCommands(names, label) {
  assert.deepEqual(sent.map((x) => x.name), names, label);
  sent.length = 0;
}

// Normal manual start is available only in ready OFF state.
paint(ready());
assert.equal(buttons["m-start"].disabled, false);
click("m-start");
assertCommands(["power_on"], "start when ready");

paint(heating());
assert.equal(buttons["m-start"].disabled, true);
click("m-start");
assertCommands([], "start while already running");
click("m-stop");
assertCommands(["off"], "stop while running");

// Critical pre-press race: another actor stops the run before the user touches
// the old Stop location. Stop remains Stop; it can never energize the heater.
paint(heating());
paint(ready());
click("m-stop");
assertCommands(["off"], "stop after another actor turns heater off");

// A fault or inhibit before the press leaves Stop available, disables Start,
// and never sends power_on.
for (const [label, state] of [["fault", fault()], ["inhibit", inhibited()]]) {
  paint(state);
  assert.equal(buttons["m-start"].disabled, true, label);
  assert.equal(buttons["m-stop"].disabled, false, label);
  click("m-start");
  click("m-stop");
  assertCommands(["off"], label);
}
assert.match(statuses.at(-2), /Heater off.*fault latched/);
assert.match(statuses.at(-1), /Heating unavailable/);

// State changes between a rendered Start and click are checked again in the
// click handler, even if an asynchronously delivered snapshot has not rendered.
paint(ready());
context.last = fault();
click("m-start");
assertCommands([], "start blocked by newer fault snapshot");
paint(ready());
context.last = heating();
click("m-start");
assertCommands([], "start blocked by newer heating snapshot");

// Product policy permits a manual run to override AUTO or DRYING. AUTO
// retains its product-local resume policy; the shared UI must not block either.
for (const mode of ["auto", "drying"]) {
  paint(otherMode(mode));
  assert.equal(buttons["m-start"].disabled, false, mode);
  click("m-start");
  assertCommands(["power_on"], mode + " permits manual override");
  click("m-stop");
  assertCommands(["off"], mode + " Stop remains OFF");
}

// Even when Start is enabled for AUTO/DRYING, a fresh safety fault or an
// intervening manual run must prevent an energizing request at click time.
for (const mode of ["auto", "drying"]) {
  paint(otherMode(mode));
  context.last = fault();
  click("m-start");
  assertCommands([], mode + " fault arrives before Start");
  paint(otherMode(mode));
  context.last = heating();
  click("m-start");
  assertCommands([], mode + " manual run arrives before Start");
}

// Unknown modes are not silently treated as permission to start heating.
paint(otherMode("unknown"));
assert.equal(buttons["m-start"].disabled, true);
click("m-start");
assertCommands([], "unknown mode blocks manual Start");

// No device state cannot authorize a manual Start. Explicit Stop remains a
// harmless idempotent OFF request as far as the browser is concerned.
paint(ready());
context.last = null;
click("m-start");
click("m-stop");
assertCommands(["off"], "missing snapshot");

// Stable Stop works for keyboard and pointer-generated click events alike;
// there is no intent state to leak from abandoned gestures.
paint(ready());
click("m-stop");
click("m-stop");
assertCommands(["off", "off"], "repeated Stop");

console.log("dc_ui separate manual Start/Stop regression: PASS");
