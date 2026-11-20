import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const html=fs.readFileSync(new URL('../components/dc_ui/www/app.html',import.meta.url),'utf8');
function section(start,end){
  const a=html.indexOf(start),b=html.indexOf(end,a);
  assert.ok(a>=0&&b>a,'missing script section '+start);
  return html.slice(a,b);
}
assert.match(html,/<div class="app" data-state="off" data-online="0" hidden>/,
  'DragonBreath surface must be hidden before device identity arrives');
assert.match(html,/id="pwr-app"[^>]* hidden>/,'DragonPWR starts hidden');
assert.match(html,/id="dc-token-card"[^>]* hidden>/,'token card remains gated');

const tokenCard=section('id="dc-token-card"','id="dc-maintenance"');
assert.doesNotMatch(tokenCard,/heater|Klipper|Moonraker/i,
  'shared setup should not describe unrelated heater integrations');
const saveToken=section('  function saveToken(clear){',"  $('s-token-save').addEventListener");
assert.doesNotMatch(saveToken,/heater|Klipper|Moonraker/i,
  'token confirmation must be product-neutral');

const code=section('  function applyDeviceInfo(i){','  /* ---- Appearance:');
function setup(){
  const app={hidden:true},ventApp={hidden:true},statusApp={hidden:true},
    wheezeApp={hidden:true},pwrApp={hidden:true};
  const nodes={};const $=id=>(nodes[id]??={hidden:false,textContent:''});
  const context={app,ventApp,statusApp,wheezeApp,pwrApp,product:'dragonbreath',
    pwrBoot:null,pwrCaps:{},pwrCapsBoot:null,pwrInfoVerified:false,pwrInfoRetryAt:0,
    document:{title:'DragonBreath',querySelectorAll:()=>[],querySelector:()=>null},
    $,u:(id,v)=>{$(id).textContent=v},activatePage:()=>{},fetch:()=>Promise.reject()};
  vm.createContext(context);
  vm.runInContext(code,context);
  return context;
}
{
  const c=setup();
  assert.equal(c.app.hidden,true,'no heater before identity');
  c.applyDeviceInfo({firmware:'test',boot_id:'A',ui:{schema:1,product:'dragonpwr'},
    capabilities:['polling']});
  assert.equal(c.app.hidden,true,'DragonPWR never reveals heater view');
  assert.equal(c.pwrApp.hidden,false,'DragonPWR view appears after descriptor');
}
{
  const c=setup();
  c.applyDeviceInfo({firmware:'test',ui:{schema:1,product:'dragonbreath'}});
  assert.equal(c.app.hidden,false,'known DragonBreath shows dashboard');
}
{
  const c=setup();
  c.applyDeviceInfo({firmware:'old',ui:{}});
  assert.equal(c.app.hidden,false,'legacy descriptor still opens DragonBreath');
}
{
  const c=setup();
  c.applyDeviceInfo({firmware:'unknown',ui:{schema:1,product:'unrecognized'}});
  assert.equal(c.app.hidden,true,'unknown product never reveals heater UI');
}
{
  const c=setup();
  c.applyDeviceInfo({firmware:'new',ui:{schema:9,product:'dragonbreath'}});
  assert.equal(c.app.hidden,true,'unrecognized schema stays hidden');
}
const eventCode=section('  function events(){','  /* ---- Navigation:');
{
  let polled=0,eventsStarted=0;
  const c={product:'dragonpwr',poll:()=>{polled++},EventSource:function(){eventsStarted++}};
  vm.createContext(c);vm.runInContext(eventCode,c);c.events();
  assert.equal(polled,1,'polling-only DragonPWR starts state polling');
  assert.equal(eventsStarted,0,'DragonPWR must not attempt unsupported SSE');
}
console.log('dc_ui product startup, token copy and SSE gating: PASS');
