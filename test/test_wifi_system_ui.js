// Exercise the shipped source functions with controlled network responses.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname,'../include/web/mcp2515_dashboard_ui.src.h'),'utf8');
function extract(name) {
  const match = new RegExp('^(?:async )?function '+name+'\\(','m').exec(source);
  assert(match, name);
  const tail = source.slice(match.index+1);
  const next = /\n(?:async )?function \w+\(/.exec(tail);
  assert(next, name+' end');
  return source.slice(match.index, match.index+1+next.index);
}
function context(response) {
  const elements = new Map();
  const element = id => {
    if(!elements.has(id)) elements.set(id,{textContent:'',innerHTML:'',disabled:false,style:{},querySelectorAll:()=>[]});
    return elements.get(id);
  };
  const requests=[];
  const c = vm.createContext({
    console,AbortController,setTimeout,clearTimeout,
    $:element,setText:(id,text)=>{element(id).textContent=text;},trText:s=>s,
    escapeHtml:s=>String(s).replace(/</g,'&lt;'),rssiIcon:()=>'',wifiAuthLabel:()=>'',
    fetch:async(url)=>{requests.push(url);if(response instanceof Error)throw response;return {ok:response.httpOk!==false,json:async()=>response};},
    fetchPollJson:async()=>response,runPoll:async(name,fn)=>fn(),
    systemStatusEnabled:false,fmtBytes:n=>String(n||0),pct:()=>0,fmtUp:n=>n+'s',fmtAddr:n=>'0x'+Number(n).toString(16),setFill:()=>{},
  });
  for(const name of ['scanWifi','loadSystemStatus']) vm.runInContext(extract(name),c);
  return {c,elements,element,requests};
}
(async()=>{
  let t=context({ok:true,networks:[{ssid:'Phone<Hotspot>',rssi:-40,ch:6,enc:true,auth:3}]});
  await t.c.scanWifi();
  assert.equal(t.requests[0],'/wifi_scan?force=1');
  assert.match(t.element('wifi-nets').innerHTML,/Phone&lt;Hotspot>/);
  assert.equal(t.element('scan-btn').disabled,false);
  t=context({ok:false,networks:[],error:'manual-scan-required'});
  await t.c.scanWifi();
  assert.match(t.element('wifi-status').textContent,/manual-scan-required/);
  assert.doesNotMatch(t.element('wifi-nets').innerHTML,/No networks found/);
  t=context({httpOk:false,ok:false,error:'ESP_ERR_WIFI_STATE'});
  await t.c.scanWifi();
  assert.match(t.element('wifi-status').textContent,/WiFi is connecting/);
  assert.equal(t.element('scan-btn').disabled,false);
  const timeout = new Error('aborted');timeout.name='AbortError';
  t=context(timeout);await t.c.scanWifi();
  assert.match(t.element('wifi-status').textContent,/Scan timed out/);
  assert.equal(t.element('scan-btn').disabled,false);
  t=context({ok:true,cached:true,networks:[]});await t.c.scanWifi();
  assert.equal(t.element('wifi-status').textContent,'Recent scan results');
  t=context({chip:'ESP32-S3',cpu_mhz:240,cores:2,heap_total:1000,heap_free:500,psram_total:8388608,psram_free:8000000,priorityMode:true,firmware:'3.0.0-beta.6'});
  await t.c.loadSystemStatus(true); // first snapshot works while monitoring is off
  assert.match(t.element('sys-summary').textContent,/ESP32-S3.*2 cores.*240 MHz/);
  assert.match(t.element('sys-psram').textContent,/8000000/);
  assert.match(t.element('sys-fw').textContent,/3.0.0-beta.6/);
  t=context({priorityMode:true});await t.c.loadSystemStatus(true);
  assert.equal(t.element('sys-summary').textContent,'System status unavailable');
  const startup=[];
  const c=vm.createContext({resetSystemStatusUi:()=>startup.push('reset'),loadSystemStatus:once=>startup.push(['system',once]),loadTaskStats:once=>startup.push(['tasks',once])});
  vm.runInContext(extract('initSystemMonitor'),c);c.initSystemMonitor();
  assert.deepEqual(startup,['reset',['system',true],['tasks',true]]);
  t=context({count:2,max:4,active:1,networks:[{idx:0,ssid:'Home'},{idx:1,ssid:'Phone-AP'}]});
  Object.assign(t.c,{wifiSlotCache:{count:0,max:4,active:-1,networks:[]},wifiStatusCache:{connected:true,ssid:'Phone-AP'},wifiSlotsLoaded:false,wifiSlotsError:false});
  vm.runInContext(extract('renderWifiSlots'),t.c);
  vm.runInContext(extract('loadWifiNetworks'),t.c);
  await t.c.loadWifiNetworks();
  assert.equal(t.element('wifi-slot-count').textContent,'(2/4)');
  assert.match(t.element('wifi-saved-list').innerHTML,/Phone-AP/);
  assert.match(t.element('wifi-saved-list').innerHTML,/\[connected\]/);
  t.c.fetchPollJson=async()=>({connected:true,ssid:'Phone-AP',count:2,active:1,ip:'10.0.0.23'});
  vm.runInContext(extract('loadWifiStatus'),t.c);
  await t.c.loadWifiStatus();
  assert.match(t.element('wifi-status').textContent,/Connected: Phone-AP/);
  assert.doesNotMatch(t.element('wifi-status').textContent,/switch to/);
  t.c.wifiSlotsLoaded=false;t.c.fetchPollJson=async()=>{throw new Error('offline');};
  await t.c.loadWifiNetworks();
  assert.equal(t.element('wifi-saved-list').textContent,'Saved networks unavailable');
  const scheduled=[];
  const polling=vm.createContext({networkPerformanceMode:true,isCarUiActive:()=>false,dashboardPollTimers:[],clearDashboardPollingIntervals:()=>{},intervalVisible:(fn)=>scheduled.push(fn.name),poll(){},loadWifiStatus(){},loadApStatus(){},loadGatewayStatus(){},loadWifiNetworks(){},loadGatewayBlocked(){},loadGatewayDns(){},updateNetworkPerformanceUi(){}});
  vm.runInContext(extract('startDashboardPolling'),polling);polling.startDashboardPolling();
  assert(scheduled.includes('loadWifiNetworks'));
  console.log('PASS: 12 WiFi scan/system status/saved network UI scenarios');
})().catch(e=>{console.error(e);process.exitCode=1;});
