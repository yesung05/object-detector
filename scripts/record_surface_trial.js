/* Read-only live observation recorder. No model, camera, or config mutations. */
const fs=require('node:fs'),path=require('node:path'),readline=require('node:readline');
const {performance}=require('node:perf_hooks');
function argumentsFor(argv){
 const opts={url:'http://localhost:8081',surface:'table-1',seconds:240,interval:500,scenario:'unlabeled',source:'live',evidence:'off'};
 for(let i=0;i<argv.length;i+=2){const key=argv[i].replace(/^--/,'');if(!(key in opts)||argv[i+1]===undefined)throw Error('Unknown/missing option: '+argv[i]);opts[key]=argv[i+1];}
 for(const key of ['seconds','interval'])opts[key]=Number(opts[key]);
 if(!Number.isFinite(opts.seconds)||opts.seconds<1||opts.seconds>3600||!Number.isFinite(opts.interval)||opts.interval<250||opts.interval>5000)throw Error('seconds: 1..3600; interval: 250..5000 ms');
 const url=new URL(opts.url);if(!['http:','https:'].includes(url.protocol)||url.username||url.password)throw Error('Use an HTTP(S) URL without credentials');
 if(!['live','test-fixture'].includes(opts.source))throw Error('source must be live or test-fixture');
 if(!['on','off'].includes(opts.evidence))throw Error('evidence must be on or off');
 opts.url=url.origin;return opts;
}
function schedulerDelta(first,last,invalid){
 if(invalid||!first||!last)return null;
 const result={};for(const key of ['requests','audits','reuse_observations']){
  if(!Number.isFinite(first[key])||!Number.isFinite(last[key])||last[key]<first[key])return null;
  result[key]=last[key]-first[key];
 }return result;
}
async function record(opts){
 const outputRoot=path.resolve(__dirname,'../runs/surface-experiments');fs.mkdirSync(outputRoot,{recursive:true});
 const directory=fs.mkdtempSync(path.join(outputRoot,'trial-'));
 const samplesPath=path.join(directory,'samples.jsonl'),labelsPath=path.join(directory,'labels.jsonl');
 const start=performance.now(),startedAt=new Date().toISOString(),elapsed=()=>Number(((performance.now()-start)/1000).toFixed(3));
 const append=(file,item)=>fs.appendFileSync(file,JSON.stringify(item)+'\n');
 let stop=false,ok=0,failed=0,first=null,last=null,firstRevision=null,lastFrame=null,changedAt=0,invalid=false,previousAlert=null,previousSample=null;
 const labels=[],onsets=[],warnings=new Set();
 const evidence=opts.evidence==='on'?require('./surface_evidence').createEvidence(directory,opts,elapsed):null;
 const input=readline.createInterface({input:process.stdin});
 const names={p:'placed',l:'person_left',v:'visible_again',r:'removed',n:'clean_confirmed'};
 const finish=()=>{stop=true;};process.once('SIGINT',finish);
 input.on('line',line=>{const key=line.trim().toLowerCase();if(key==='q'){stop=true;return;}if(!names[key]){console.log('Use p/l/v/r/n then Enter, or q to stop.');return;}const marker={label:names[key],elapsed_s:elapsed(),wall_time:new Date().toISOString(),origin:'manual_keypress'};labels.push(marker);append(labelsPath,marker);console.log('Marked:',marker.label,marker.elapsed_s);});
 fs.writeFileSync(path.join(directory,'manifest.json'),JSON.stringify({started_at:startedAt,...opts,poll_clock:'recorder monotonic seconds',note:'Manual labels require verification. Optional asynchronous JPEG evidence is not continuous video. No automatic precision/recall.'},null,2));
 console.log('Recording:',directory);console.log('p=placed, l=person left, v=visible again, r=removed, n=clean confirmed, q=stop; press Enter.');
 try{
  while(!stop&&elapsed()<opts.seconds){
   const sent=elapsed();
   try{
    const response=await fetch(opts.url+'/surface/status',{signal:AbortSignal.timeout(3000)});if(!response.ok)throw Error('HTTP '+response.status);
    const data=await response.json(),received=elapsed(),s=(data.surfaces||[]).find(s=>s.id===opts.surface);
    if(!s)throw Error('Target surface not found: '+opts.surface);
    if(firstRevision===null)firstRevision=data.revision;else if(data.revision!==firstRevision){invalid=true;warnings.add('Configuration revision changed: do not compare counter totals.');}
    if(lastFrame!==null&&data.frame_time<lastFrame){invalid=true;warnings.add('Detector clock reset.');}
    if(data.frame_time!==lastFrame){changedAt=received;lastFrame=data.frame_time;}
    const stale=!Number.isFinite(data.frame_time)||received-changedAt>4||s.age>3;
    if(stale)warnings.add('Stale observations occurred.');
    if(!data.enabled||!s.baseline||s.capturing)warnings.add('Disabled or baseline not ready for part of the recording.');
    if(!data.ai_scheduler)warnings.add('AI scheduler metrics unavailable: upgrade detector before efficiency comparison.');
    const current=data.ai_scheduler||null;
    if(ok===0)first=current;
    if(last&&current&&current.requests<last.requests){invalid=true;warnings.add('AI counters reset.');}
    last=current;ok++;
    const alert=(s.candidates||[]).some(c=>c.alert),row={sent_elapsed_s:sent,received_elapsed_s:received,rtt_s:Number((received-sent).toFixed(3)),received_at:new Date().toISOString(),frame_time:data.frame_time,revision:data.revision,enabled:data.enabled,stale,ai_scheduler:current,surface:s};
    append(samplesPath,row);
    if(alert&&previousAlert===false)onsets.push({first_seen_s:received,previous_sample_s:previousSample,stale,gap_note:'HTTP observation interval, not exact detector event time'});
    if(previousAlert===null&&alert)warnings.add('An alert existed at the first sample; it is not a new event.');
    previousAlert=alert;previousSample=received;
    if(evidence)await evidence.observe(row);
   }catch(e){failed++;warnings.add('Polling errors occurred; onset intervals may contain gaps.');append(samplesPath,{sent_elapsed_s:sent,received_elapsed_s:elapsed(),error:e.message});}
   const remaining=Math.min(opts.interval/1000-(elapsed()-sent),opts.seconds-elapsed());if(remaining>0&&!stop)await new Promise(r=>setTimeout(r,remaining*1000));
  }
 }finally{
  input.close();process.removeListener('SIGINT',finish);
  const summary={data_source:opts.source,scenario:opts.scenario,duration_s:elapsed(),samples_ok:ok,samples_failed:failed,manual_labels:labels,observed_alert_onsets:onsets,ai_scheduler_delta:schedulerDelta(first,last,invalid),warnings:[...warnings],accuracy_evaluated:false,cpu_measured:false,actual_ai_runs_measured:false,note:'Onsets are polling observations, not TP/FP labels or exact alert latency. Match to synchronized video before reporting accuracy.'};
  if(evidence)summary.evidence=evidence.finish();
  fs.writeFileSync(path.join(directory,'summary.json'),JSON.stringify(summary,null,2));console.log(JSON.stringify(summary,null,2));console.log('Saved:',directory);
 }
 return directory;
}
if(require.main===module){if(process.argv.includes('--help'))console.log('node scripts/record_surface_trial.js --surface table-1 --scenario table_residue --seconds 240 --interval 500\nRead-only polling, no camera recording. Press p/l/v/r/n + Enter to mark actions; q + Enter to stop.');else{let opts;try{opts=argumentsFor(process.argv.slice(2));}catch(e){console.error(e.message);process.exitCode=1;}if(opts)record(opts).catch(e=>{console.error(e);process.exitCode=1;});}}
module.exports={argumentsFor,schedulerDelta,record};
