// Run with: node --input-type=module -e "(await import('./tests/reset/ui_test.mjs')).default(process.cwd())"
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

export default async function testResetUI(root) {
 const html=readFileSync(root+'/data/www/index.html','utf8');
 const script=[...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].map(x=>x[1]).join('\n');
 function setup({confirm=true,prompt=null,result={success:true,restarting:true},ok=true,text=null,reject=false,hold=false}={}) {
  const elements=Object.fromEntries(['reset-homekit-btn','reset-factory-btn','reset-msg'].map(id=>[id,{disabled:false,textContent:'',className:''}]));
  const calls=[];let resolve;
  const context=vm.createContext({document:{getElementById:id=>elements[id]},window:{},confirm:()=>confirm,prompt:()=>prompt,
   fetch:async (url,options)=>{calls.push({url,options});if(reject)throw new Error('network');if(hold)await new Promise(r=>resolve=r);return {ok,status:ok?200:503,text:async()=>text===null?JSON.stringify(result):text};},setTimeout:()=>{},setInterval:()=>{}});
  new vm.Script(script,{filename:'index.html'}).runInContext(context);
  assert.equal(typeof context.resetDevice,'function','Reset action must be implemented');
  return {context,elements,calls,release:()=>resolve()};
 }
 let s=setup({confirm:false});await s.context.resetDevice('homekit');assert.equal(s.calls.length,0);
 for(const prompt of [null,'reset','WRONG']){s=setup({prompt});await s.context.resetDevice('factory');assert.equal(s.calls.length,0);}
 s=setup();await s.context.resetDevice('other');assert.equal(s.calls.length,0);
 console.log('PASS canceled, mistyped and unknown reset choices never send a request');
 s=setup({hold:true});const pending=s.context.resetDevice('homekit');assert.equal(s.calls.length,1);assert(s.elements['reset-homekit-btn'].disabled&&s.elements['reset-factory-btn'].disabled);await s.context.resetDevice('factory');assert.equal(s.calls.length,1);s.release();await pending;
 assert.equal(s.calls[0].url,'/api/system/reset');assert.equal(s.calls[0].options.method,'POST');assert.equal(s.calls[0].options.headers['Content-Type'],'application/json');assert.deepEqual(JSON.parse(s.calls[0].options.body),{mode:'homekit',confirm:'RESET HOMEKIT'});assert.match(s.elements['reset-msg'].textContent,/Home/);assert(s.elements['reset-homekit-btn'].disabled);
 console.log('PASS HomeKit explicit POST, disabled actions and duplicate-click suppression through restart');
 s=setup({prompt:'ERASE ALL'});await s.context.resetDevice('factory');assert.deepEqual(JSON.parse(s.calls[0].options.body),{mode:'factory',confirm:'ERASE ALL'});assert.match(s.elements['reset-msg'].textContent,/Wi-Fi/);
 console.log('PASS factory reset requires typed confirmation and explains setup after reboot');
 s=setup({ok:false,text:'Still starting. Try again shortly.'});await s.context.resetDevice('homekit');assert.match(s.elements['reset-msg'].textContent,/Still starting/);assert(!s.elements['reset-homekit-btn'].disabled);
 s=setup({ok:false,result:{success:false,restarting:true,error:'Reset incomplete. Restarting; retry after reboot.'}});await s.context.resetDevice('factory');assert.equal(s.calls.length,0); // missing typed confirmation
 s=setup({prompt:'ERASE ALL',ok:false,result:{success:false,restarting:true,error:'Reset incomplete. Restarting; retry after reboot.'}});await s.context.resetDevice('factory');assert.match(s.elements['reset-msg'].textContent,/incomplete/);assert(s.elements['reset-factory-btn'].disabled);
 s=setup({reject:true});await s.context.resetDevice('homekit');assert.match(s.elements['reset-msg'].textContent,/not confirmed/);
 console.log('PASS server/partial-erase/network failures never display successful reset');
}
