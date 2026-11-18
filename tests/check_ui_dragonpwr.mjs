import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const html = fs.readFileSync(new URL("../components/dc_ui/www/app.html", import.meta.url), "utf8");
function section(start,end){
  const a=html.indexOf(start),b=html.indexOf(end,a);
  assert.ok(a>=0&&b>a,"missing section: "+start);
  return html.slice(a,b);
}

// Exercise the actual production functions, not copies or string-marker mocks.
const code=section("  function applyDeviceInfo(i){","  /* ---- Appearance:")
  + "\n" + section("  function pwrSetConn(connected){","  /* ---- DragonVent adapter.");

function makeElement(extra={}){
  return {hidden:false,className:"",textContent:"",dataset:{},...extra};
}
function setup(){
  const ids=["pwr-app","pwr-conn","pwr-mains","pwr-usb","pwr-controls",
    "pwr-note","pwr-restore","pwr-free","pwr-min","pwr-largest",
    "pwr-meter","pwr-printer","pwr-version","pwr-footer-version",
    "pwr-device","pwr-boot","pwr-cap"];
  const elements=Object.fromEntries(ids.map(id=>[id,makeElement()]));
  elements["pwr-app"].hidden=true;
  elements["pwr-controls"].hidden=true;
  const context={
    app:makeElement(),ventApp:makeElement(),statusApp:makeElement(),
    wheezeApp:makeElement(),pwrApp:elements["pwr-app"],pwrCaps:{},
    product:"dragonbreath",
    document:{title:"DragonBreath"},
    $:(id)=>{assert.ok(elements[id],"unknown DOM element: "+id);return elements[id];},
    u:(id,value)=>{if(elements[id])elements[id].textContent=value;},
    activatePage:()=>{throw Error("unexpected DragonBreath page activation");}
  };
  vm.createContext(context);
  vm.runInContext(code,context,{filename:"dc_ui-dragonpwr.js"});
  return {context,elements};
}

const c3={outputs:{mains:null,usb1:null},restore:null,meter:null,printer:null,
  heap:{free:172924,min_free:141724,largest_block:114688}};
const c2={outputs:{mains:true,usb1:false},restore:"off",meter:null,printer:null,
  heap:{free:100000,min_free:90000,largest_block:80000}};

// F1: /api/v2/info fails; /api/v2/state must select DragonPWR rather than
// leave visible, enabled heater controls. Synthesized identity has no capabilities.
{
  const {context:c,elements:e}=setup();
  c.applyPwr(c3);
  assert.equal(e["pwr-app"].hidden,false);
  assert.equal(c.app.hidden,true);
  assert.equal(c.ventApp.hidden,true);
  assert.equal(c.statusApp.hidden,true);
  assert.equal(c.wheezeApp.hidden,true);
  assert.equal(e["pwr-controls"].hidden,true);
  assert.equal(e["pwr-mains"].textContent,"Unavailable");
  assert.equal(e["pwr-usb"].textContent,"Unavailable");
}
// Capabilities without real boolean output states must never expose controls.
{
  const {context:c,elements:e}=setup();
  c.applyDeviceInfo({firmware:"test",ui:{schema:1,product:"dragonpwr"},
    capabilities:["power_switch","usb_switch"]});
  c.applyPwr(c3);
  assert.equal(e["pwr-controls"].hidden,true);
  assert.equal(e["pwr-mains"].textContent,"Unavailable");
}
// F2: stale logical states must not remain visible/actionable on disconnect,
// and a subsequent good snapshot must restore the live labels.
{
  const {context:c,elements:e}=setup();
  c.applyDeviceInfo({firmware:"test",ui:{schema:1,product:"dragonpwr"},
    capabilities:["power_switch","usb_switch"]});
  c.applyPwr(c2);
  assert.equal(e["pwr-mains"].textContent,"On");
  assert.equal(e["pwr-usb"].textContent,"Off");
  assert.equal(e["pwr-controls"].hidden,false);
  c.pwrSetConn(false);
  assert.match(e["pwr-mains"].textContent,/disconnected/i);
  assert.match(e["pwr-usb"].textContent,/disconnected/i);
  assert.equal(e["pwr-controls"].hidden,true);
  c.applyPwr(c2);
  assert.equal(e["pwr-mains"].textContent,"On");
  assert.equal(e["pwr-controls"].hidden,false);
}
// F4: missing heap must not suppress rendering of valid power state.
{
  const {context:c,elements:e}=setup();
  c.applyPwr({outputs:{mains:null,usb1:null},restore:null});
  assert.equal(e["pwr-app"].hidden,false);
  assert.equal(e["pwr-free"].textContent,"—");
  assert.equal(e["pwr-mains"].textContent,"Unavailable");
}
// Product-specific shared view must not post power commands.
const surface=section('<div class="app pwr-app"','<section class="provision-overlay"');
assert.ok(!/fetch\s*\(|method\s*:\s*['"]POST['"]|\/api\/v2\/command/.test(surface),
  "DragonPWR view must stay read-only");

console.log("dc_ui DragonPWR state-selection and disconnect behavior: PASS");
