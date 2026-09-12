const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const src=fs.readFileSync(__dirname+'/../include/web/mcp2515_dashboard_ui.src.h','utf8');
const start=src.indexOf('async function uploadFirmware()');
const tail=src.slice(start+1); const end=/\n(?:async )?function \w+\(/.exec(tail);
assert(start>=0 && end);
const elements=new Map(); const el=id=>{if(!elements.has(id))elements.set(id,{style:{},textContent:'',disabled:false});return elements.get(id);};
let xhr;
const ctx=vm.createContext({
  $:el,otaFile:{name:'firmware.bin',size:1024},console,
  prompt:()=>{throw new Error('OTA must not prompt for credentials');},
  localStorage:{getItem:()=>{throw new Error('must not read credentials');},setItem:()=>{throw new Error('must not save credentials');}},
  XMLHttpRequest:class{constructor(){this.upload={};this.headers={};xhr=this;}open(...args){this.args=args;}setRequestHeader(k,v){this.headers[k]=v;}send(file){this.file=file;}},
  setTimeout:()=>0,location:{reload(){}},trText:x=>x,
});
vm.runInContext(src.slice(start,start+1+end.index),ctx);
(async()=>{
  await ctx.uploadFirmware();
  assert.deepEqual(xhr.args,['POST','/update',true]);
  assert.equal(xhr.file,ctx.otaFile);assert.equal(xhr.headers.Authorization,undefined);
  assert.equal(xhr.headers['Content-Type'],'application/octet-stream');
  assert.doesNotMatch(src,/otaUser|otaPass|resetOtaCredentials|id="ota-reset-btn"/);
  console.log('PASS: OTA upload needs no prompt, credential storage or Authorization header');
})().catch(e=>{console.error(e);process.exitCode=1;});
