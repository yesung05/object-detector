const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {argumentsFor,schedulerDelta,record}=require('./record_surface_trial');
async function main(){
 assert.throws(()=>argumentsFor(['--seconds','-1']));assert.throws(()=>argumentsFor(['--url','file:///tmp']));assert.throws(()=>argumentsFor(['--surface']));
 assert.equal(schedulerDelta(null,null,false),null);
 assert.equal(schedulerDelta({requests:9,audits:0,reuse_observations:2},{requests:1,audits:0,reuse_observations:0},false),null);
 assert.deepEqual(schedulerDelta({requests:1,audits:0,reuse_observations:2},{requests:3,audits:1,reuse_observations:8},false),{requests:2,audits:1,reuse_observations:6});
 const previous=global.fetch;let calls=0;
 global.fetch=async()=>{calls++;if(calls===2)throw Error('test disconnection');return {ok:true,json:async()=>({enabled:true,revision:1,frame_time:calls,surfaces:[{id:'table-1',baseline:true,quality:0,age:0,occupancy:'vacant',candidates:calls>=3?[{alert:true}]:[]}]})};};
 try{
  const dir=await record(argumentsFor(['--source','test-fixture','--scenario','recorder_test','--seconds','1','--interval','250']));
  assert(dir.startsWith(path.resolve(__dirname,'../runs/surface-experiments')+path.sep));
  const summary=JSON.parse(fs.readFileSync(path.join(dir,'summary.json'),'utf8'));
  assert.equal(summary.data_source,'test-fixture');assert.equal(summary.samples_failed,1);assert.equal(summary.ai_scheduler_delta,null);assert.equal(summary.accuracy_evaluated,false);assert.equal(summary.observed_alert_onsets.length,1);
  const samples=fs.readFileSync(path.join(dir,'samples.jsonl'),'utf8').trim().split('\n').map(JSON.parse);assert.equal(samples.length,summary.samples_ok+summary.samples_failed);
 }finally{global.fetch=previous;}
 console.log('Trial recorder validation: PASS (synthetic HTTP fixture, not detection accuracy)');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
