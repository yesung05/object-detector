const assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {createEvidence}=require('./surface_evidence');
async function main(){
 const dir=fs.mkdtempSync(path.join(os.tmpdir(),'surface-evidence-'));
 let time=0,calls=0;const original=global.fetch;
 global.fetch=async()=>{calls++;return {ok:true,arrayBuffer:async()=>Buffer.from([255,216,255,217])};};
 const recorder=createEvidence(dir,{url:'http://fixture'},()=>time);
 const row={received_elapsed_s:0,enabled:true,revision:1,stale:false,surface:{baseline:true,quality:0,occupancy:'vacant',candidates:[]}};
 try{
  await recorder.observe(row);
  time=1;row.received_elapsed_s=time;row.surface.candidates=[{alert:true}];await recorder.observe(row);
  assert.equal(calls,1);
  time=5;row.received_elapsed_s=time;await recorder.observe(row);assert.equal(calls,2);
  time=10;row.received_elapsed_s=time;global.fetch=async()=>{throw Error('<offline>');};await recorder.observe(row);
  const summary=recorder.finish();assert.equal(summary.events,4);assert.equal(summary.images,2);
  const html=fs.readFileSync(path.join(dir,'review.html'),'utf8');assert(html.includes('&lt;offline&gt;'));assert(html.includes('images/00001.jpg'));
  assert.equal(fs.readFileSync(path.join(dir,'events.jsonl'),'utf8').trim().split('\n').length,4);
  console.log('Evidence test PASS (synthetic, not camera accuracy)');
 }finally{global.fetch=original;}
}
main().catch(e=>{console.error(e);process.exitCode=1;});
