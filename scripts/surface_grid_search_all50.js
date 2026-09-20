/* 50-video grid search for surface contamination detection.
 * Uses expected_alert/alert/valid field names (different from 21-video expected_residue format).
 * S33 is evaluated but excluded from primary ranking (protocol-changed: cup present from start).
 * Cases with expected_alert=null are excluded from metrics and ranking denominator.
 * Path migration: --project-root replaces the old project root prefix in manifest paths.
 * --no-rest: skip all inter-video and inter-candidate rests (use on development machines, not i5-4200U).
 * --parallel N: run N candidates concurrently (default 1). Safe: Node.js single-thread ensures atomic writes.
 * Never applies results to operational config; all outputs are development-set tuning scores. */
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),{spawn}=require('node:child_process');
const root=path.resolve(__dirname,'..'),searchRoot=path.join(root,'runs','surface-search-all50');
const ffmpeg='C:/dev/ffmpeg-master-latest-win64-gpl-shared/bin/ffmpeg.exe';
const grid={threshold:[16,24,32],min_area:[0.001,0.003],confirm_seconds:[5,10,15]};
// Constraints relative to initial A baseline (FP=12, quality=0.96). Define before seeing tuning results.
const constraints={max_false_positives:12,min_quality_coverage:0.96};
const pacing={between_videos_ms:10000,between_candidates_ms:60000};
// S33: cup present from start, not a clean-baseline-then-placement run. Tracked but excluded from ranking.
const PROTOCOL_CHANGED_IDS=new Set(['S33']);

async function rest(ms,dir,context){if(!ms)return;write(path.join(dir,'progress.json'),{state:'resting',...context,updated_at:new Date().toISOString(),next_start_after:new Date(Date.now()+ms).toISOString()});console.log('Rest: '+ms/1000+'s');await new Promise(r=>setTimeout(r,ms));}
async function exclusiveSearch(action){const server=require('node:net').createServer(s=>s.end());await new Promise((resolve,reject)=>{server.once('error',()=>reject(Error('Another all50 search is running or port 8094 is unavailable.')));server.listen(8094,'127.0.0.1',resolve);});try{return await action();}finally{await new Promise(r=>server.close(r));}}
async function hashFile(file){const h=crypto.createHash('sha256');for await(const chunk of fs.createReadStream(file))h.update(chunk);return h.digest('hex');}
function digest(value){return crypto.createHash('sha256').update(JSON.stringify(value)).digest('hex');}
function read(file){return JSON.parse(fs.readFileSync(file,'utf8'));}
function write(file,value){const temp=file+'.'+crypto.randomUUID()+'.tmp';fs.writeFileSync(temp,JSON.stringify(value,null,2));fs.renameSync(temp,file);}

// Resolve a path recorded under an old project root to the current one. Handles D:\ → G:\ migrations.
function resolvePath(recorded,projectRoot){
  if(fs.existsSync(recorded))return recorded;
  if(!projectRoot)throw Error('File not found: '+recorded+'. Specify --project-root PATH.');
  const normalized=recorded.replace(/\\/g,'/');
  for(const anchor of ['/records/','/runs/','/config/']){
    const idx=normalized.indexOf(anchor);
    if(idx>=0){const candidate=path.join(projectRoot,anchor.slice(1)+normalized.slice(idx+anchor.length));if(fs.existsSync(candidate))return candidate;}
  }
  throw Error('Cannot locate file (tried '+projectRoot+' remapping): '+recorded);
}

function variants(){
  const all=[];for(const threshold of grid.threshold)for(const min_area of grid.min_area)for(const confirm_seconds of grid.confirm_seconds)all.push({threshold,min_area,confirm_seconds});
  const initial={threshold:24,min_area:0.003,confirm_seconds:15};
  return [initial,...all.filter(p=>digest(p)!==digest(initial))].map((params,i)=>({id:'candidate-'+String(i).padStart(2,'0'),params}));
}

function metrics50(rows){
  function calc(subset){
    const counts={TP:0,FP:0,TN:0,FN:0};for(const r of subset)counts[r.expected_alert?(r.alert?'TP':'FN'):(r.alert?'FP':'TN')]++;
    const {TP,FP,TN,FN}=counts,n=subset.length;
    return{...counts,n,precision:TP+FP?TP/(TP+FP):0,recall:TP+FN?TP/(TP+FN):0,f1:2*TP+FP+FN?2*TP/(2*TP+FP+FN):0,accuracy:n?(TP+TN)/n:0,quality_coverage:n?subset.filter(r=>r.valid).length/n:0};
  }
  const eligible=rows.filter(r=>r.expected_alert!==null&&!PROTOCOL_CHANGED_IDS.has(r.id));
  const with_s33=[...eligible,...rows.filter(r=>r.expected_alert!==null&&PROTOCOL_CHANGED_IDS.has(r.id))];
  return{primary:calc(eligible),with_protocol_changed:calc(with_s33),n_excluded_null:rows.filter(r=>r.expected_alert===null).length,n_protocol_changed:rows.filter(r=>PROTOCOL_CHANGED_IDS.has(r.id)&&r.expected_alert!==null).length};
}

function rank50(entries,limits=constraints){
  return entries.filter(e=>e.complete&&e.metrics.primary.FP<=limits.max_false_positives&&e.metrics.primary.quality_coverage>=limits.min_quality_coverage&&(limits.min_recall==null||e.metrics.primary.recall>=limits.min_recall))
    .sort((a,b)=>b.metrics.primary.f1-a.metrics.primary.f1||a.metrics.primary.FP-b.metrics.primary.FP||b.metrics.primary.quality_coverage-a.metrics.primary.quality_coverage||a.id.localeCompare(b.id));
}

function configFor(base,params){
  const config=structuredClone(base);const table=config.surfaces.find(s=>s.id==='table-1');if(!table)throw Error('table-1 missing in config');
  Object.assign(table,params);return config;
}

function mergeCasesAndLabels(manifestCases,labels,projectRoot){
  const labelMap=new Map(labels.map(l=>[l.id,l]));
  return manifestCases.map(c=>{
    const lbl=labelMap.get(c.id);
    const expected_alert=lbl?.expected_alert??c.expected_alert??null;
    const video=resolvePath(c.video,projectRoot);
    return{...c,video,expected_alert,label_basis:lbl?.basis??c.label_basis,protocol_note:lbl?.protocol_note??c.protocol_note};
  });
}

async function initialize(all50Dir,projectRoot){
  fs.mkdirSync(searchRoot,{recursive:true});const dir=fs.mkdtempSync(path.join(searchRoot,'baseline-'));
  const manifest=read(path.join(all50Dir,'manifest.json')),labels=read(path.join(all50Dir,'labels.json'));
  const nullCount=labels.filter(l=>l.expected_alert===null).length;
  if(nullCount>0)console.warn('Warning: '+nullCount+' cases have expected_alert=null and will be excluded from metrics.');
  const cases=mergeCasesAndLabels(manifest.cases,labels,projectRoot);
  const exePath=resolvePath(manifest.exe,projectRoot);
  const modelPaths=manifest.models.map(m=>resolvePath(m,projectRoot));
  for(const a of manifest.artifacts){const p=resolvePath(a.file,projectRoot);const h=await hashFile(p);if(h!==a.sha256)throw Error('Artifact hash mismatch ('+path.basename(a.file)+'): expected '+a.sha256+' got '+h);}
  console.log('All artifacts verified.');
  for(const c of cases){const h=await hashFile(c.video);if(h!==c.sha256)throw Error('Video changed: '+c.id);}
  console.log('All videos verified.');
  const ffmpegHash=await hashFile(ffmpeg);
  const initialConfig=read(path.join(all50Dir,'config.json'));
  const initialResults=[];for(const c of cases){const rp=path.join(all50Dir,c.id,'result.json');if(fs.existsSync(rp))initialResults.push({...read(rp),expected_alert:c.expected_alert});}
  const initial_metrics=metrics50(initialResults.map(r=>({...r,id:r.id||'?'})));
  const frozenFiles=[];const snapshotDir=path.join(dir,'snapshot');fs.mkdirSync(snapshotDir);
  fs.copyFileSync(path.join(all50Dir,'manifest.json'),path.join(snapshotDir,'manifest.json'));
  fs.copyFileSync(path.join(all50Dir,'labels.json'),path.join(snapshotDir,'labels.json'));
  fs.copyFileSync(path.join(all50Dir,'config.json'),path.join(snapshotDir,'config.json'));
  for(const f of ['manifest.json','labels.json','config.json'])frozenFiles.push({file:'snapshot/'+f,sha256:await hashFile(path.join(snapshotDir,f))});
  const baseline={schema_version:2,created_at:new Date().toISOString(),purpose:'development tuning on 50-video all50 set; not independent test performance',all50_dir:all50Dir,project_root:projectRoot||null,old_project_root_detected:manifest.exe.replace(/\\/g,'/').match(/^(.+)\/runs\//)?.[1]||null,exe:exePath,models:modelPaths,ffmpeg,ffmpeg_sha256:ffmpegHash,artifacts_verified:true,initial_metrics,cases,frozenFiles,grid,constraints,pacing,PROTOCOL_CHANGED_IDS:[...PROTOCOL_CHANGED_IDS]};
  write(path.join(dir,'baseline.json'),baseline);
  console.log('Frozen all50 baseline: '+dir);console.log('Initial metrics (primary, excl. null+S33):',JSON.stringify(initial_metrics.primary));
  return dir;
}

async function verify(dir,baseline){
  for(const f of baseline.frozenFiles)if(await hashFile(path.join(dir,f.file))!==f.sha256)throw Error('Frozen file changed: '+f.file);
  if(await hashFile(baseline.ffmpeg)!==baseline.ffmpeg_sha256)throw Error('FFmpeg changed');
  const exeHash=await hashFile(baseline.exe);const originalExeHash=read(path.join(dir,'snapshot/manifest.json')).artifacts.find(a=>a.file.replace(/\\/g,'/').endsWith('/evaluate_surface_video.exe'))?.sha256;
  if(originalExeHash&&exeHash!==originalExeHash)throw Error('Evaluator EXE changed since baseline creation');
  for(const a of read(path.join(dir,'snapshot/manifest.json')).artifacts){const p=resolvePath(a.file,baseline.project_root);if(await hashFile(p)!==a.sha256)throw Error('Artifact changed: '+p);}
  for(const c of baseline.cases)if(await hashFile(c.video)!==c.sha256)throw Error('Video changed: '+c.id);
}

function plan(dir){
  const b=read(path.join(dir,'baseline.json'));
  const constraints=b.selection_constraints||b.constraints;
  const eligible=b.cases.filter(c=>c.expected_alert!==null&&!PROTOCOL_CHANGED_IDS.has(c.id));
  const report={baseline:dir,total_cases:b.cases.length,eligible_cases:eligible.length,null_cases:b.cases.filter(c=>c.expected_alert===null).length,protocol_changed_cases:b.cases.filter(c=>PROTOCOL_CHANGED_IDS.has(c.id)&&c.expected_alert!==null).length,candidates:variants().length,cold_video_runs:variants().length*b.cases.length,additional_video_runs:(variants().length-1)*b.cases.length,grid,constraints,ranking:'F1 descending (primary excl. S33), then fewer FP, then quality_coverage, then candidate ID',note:'S33 excluded from ranking but reported. null-expected_alert cases excluded from metrics. Operational config never changed.'};
  console.log(JSON.stringify(report,null,2));return report;
}

async function runCase(baselineDir,baseline,caseRow,candidateDir,configPath,sig){
  const work=path.join(candidateDir,caseRow.id);fs.mkdirSync(work,{recursive:true});const resultPath=path.join(work,'result.json');
  if(fs.existsSync(resultPath)){const cached=read(resultPath);if(cached.signature===sig&&cached.predictions_sha256===await hashFile(path.join(work,'predictions.jsonl')))return{...cached,expected_alert:caseRow.expected_alert};throw Error('Cached case inconsistent: '+caseRow.id);}
  const scratch=fs.mkdtempSync(path.join(work,'attempt-'));
  const prediction=path.join(work,'predictions.jsonl'),out=fs.createWriteStream(prediction),log=fs.createWriteStream(path.join(work,'process.log'));
  const ff=spawn(baseline.ffmpeg,['-hide_banner','-loglevel','error','-threads','1','-i',caseRow.video,'-an','-vf','fps=fps=2:start_time=0','-pix_fmt','rgb24','-f','rawvideo','pipe:1'],{windowsHide:true,stdio:['ignore','pipe','pipe']});
  const detector=spawn(baseline.exe,[configPath,scratch,...baseline.models,'1'],{windowsHide:true,stdio:['pipe','pipe','pipe']});
  ff.stdout.pipe(detector.stdin);detector.stdout.pipe(out);ff.stderr.pipe(log,{end:false});detector.stderr.pipe(log,{end:false});detector.stdin.on('error',()=>ff.kill());
  let stopped=false;const stop=()=>{stopped=true;ff.kill();detector.kill();};out.on('error',stop);log.on('error',stop);const timeout=setTimeout(stop,600000);process.once('SIGINT',stop);process.once('SIGTERM',stop);
  const wait=child=>new Promise((resolve,reject)=>{child.once('error',e=>{stop();reject(e);});child.once('close',code=>{if(code!==0)stop();resolve(code);});});
  try{const codes=await Promise.all([wait(ff),wait(detector)]);await new Promise((resolve,reject)=>{if(out.closed)return resolve();out.once('close',resolve);out.once('error',reject);});if(stopped||codes.some(c=>c!==0))throw Error('Evaluation interrupted/failed: '+caseRow.id);}
  finally{clearTimeout(timeout);process.removeListener('SIGINT',stop);process.removeListener('SIGTERM',stop);log.end();}
  const sequence=fs.readFileSync(prediction,'utf8').trim().split('\n').map(JSON.parse);
  if(!sequence.length||sequence.at(-1).video_s<caseRow.reference_s-0.5)throw Error('Incomplete timeline: '+caseRow.id);
  const chosen=sequence.reduce((a,b)=>Math.abs(a.video_s-caseRow.reference_s)<Math.abs(b.video_s-caseRow.reference_s)?a:b);
  if(Math.abs(chosen.video_s-caseRow.reference_s)>0.251)throw Error('Missing checkpoint: '+caseRow.id);
  const surface=chosen.status.surfaces[0];
  const processMetrics=fs.readFileSync(path.join(work,'process.log'),'utf8').match(/EVAL[^\r\n]*/)?.[0]||null;
  const result={occluder_fraction:surface.occluder_fraction??0,max_occluder_fraction:Math.max(...sequence.map(x=>x.status.surfaces[0].occluder_fraction??0)),mean_occluder_fraction:sequence.reduce((a,x)=>a+(x.status.surfaces[0].occluder_fraction??0),0)/sequence.length,occluder_rejected_samples:sequence.filter(x=>x.status.surfaces[0].occluder_rejected).length,signature:sig,id:caseRow.id,reference_s:caseRow.reference_s,prediction_s:chosen.video_s,alert:surface.candidates.some(c=>c.alert),quality:surface.quality,baseline:!!surface.baseline,valid:!!surface.baseline&&!surface.capturing&&surface.quality===0,occupancy:surface.occupancy,layout_changed:surface.layout_changed,first_alert_s:sequence.find(x=>x.status.surfaces[0].candidates.some(c=>c.alert))?.video_s??null,frames:sequence.length,process_metrics:processMetrics,predictions_sha256:await hashFile(prediction)};
  write(resultPath,result);return{...result,expected_alert:caseRow.expected_alert};
}

async function tryReuseInitial(all50Dir,caseRow,configHash,exeHash){
  if(!all50Dir)return null;const rp=path.join(all50Dir,caseRow.id,'result.json');if(!fs.existsSync(rp))return null;
  const manifest=read(path.join(all50Dir,'manifest.json'));
  const manifestExeHash=manifest.artifacts.find(a=>a.file.replace(/\\/g,'/').endsWith('/evaluate_surface_video.exe'))?.sha256;
  if(!manifestExeHash||manifestExeHash!==exeHash)return null;
  const initialConfig=read(path.join(all50Dir,'config.json'));
  if(digest(initialConfig)!==configHash)return null;
  const sourceCase=manifest.cases.find(c=>c.id===caseRow.id);if(!sourceCase||sourceCase.sha256!==caseRow.sha256||sourceCase.reference_s!==caseRow.reference_s)return null;
  const prior=read(rp);if(prior.predictions_sha256&&prior.predictions_sha256!==await hashFile(path.join(all50Dir,caseRow.id,'predictions.jsonl')))throw Error('Initial predictions changed');
  console.log('  Reusing initial result for '+caseRow.id);
  return{...read(rp),expected_alert:caseRow.expected_alert,reused_initial:true};
}

async function search(baselineDir,{resumeDir=null,smoke=false,filterIds=null,noRest=false,parallelism=1}={}){
  baselineDir=path.resolve(baselineDir);const baseline=read(path.join(baselineDir,'baseline.json'));await verify(baselineDir,baseline);
  const constraints=baseline.selection_constraints||baseline.constraints;
  const baseConfig=read(path.join(baselineDir,'snapshot/config.json'));
  // Execution definition is frozen; pacing flags may change, scientific inputs may not.
  const sig=digest({baseline,runner_sha256:await hashFile(__filename),grid,constraints,PROTOCOL_CHANGED_IDS:[...PROTOCOL_CHANGED_IDS],version:3});
  const dir=resumeDir||fs.mkdtempSync(path.join(searchRoot,smoke?'smoke-':'search-'));const manifestPath=path.join(dir,'search.json');
  if(resumeDir){const old=read(manifestPath);if(old.signature!==sig)throw Error('Search definition changed; create a new search.');}
  else write(manifestPath,{baseline:baselineDir,signature:sig,smoke,grid,constraints,noRest,parallelism,created_at:new Date().toISOString()});
  const allCandidates=smoke?[variants()[0]]:variants();
  const cohort=smoke?(filterIds?baseline.cases.filter(c=>filterIds.has(c.id)):baseline.cases.slice(0,1)):baseline.cases;
  const exeHash=await hashFile(baseline.exe);const configHash=digest(baseConfig);
  // entries[] is shared across parallel workers; safe in single-threaded Node.js (writes between awaits)
  const entries=[];console.log('Search output: '+dir+(noRest?' [no-rest]':'')+(parallelism>1?' [parallel='+parallelism+']':''));

  // Per-candidate runner: processes all videos for one candidate
  async function runCandidate(candidate){
    const candidateDir=path.join(dir,candidate.id);fs.mkdirSync(candidateDir,{recursive:true});
    const config=configFor(baseConfig,candidate.params),configPath=path.join(candidateDir,'config.json');
    if(fs.existsSync(configPath)&&digest(read(configPath))!==digest(config))throw Error('Candidate config changed: '+candidate.id);
    write(configPath,config);
    const rows=[];
    for(const c of cohort){
      const wasCached=fs.existsSync(path.join(candidateDir,c.id,'result.json'));
      if(!wasCached)write(path.join(dir,'progress.json'),{state:'running',candidate:candidate.id,scenario:c.id,updated_at:new Date().toISOString()});
      let result;
      if(!smoke&&candidate.id==='candidate-00'){
        result=await tryReuseInitial(baseline.all50_dir,c,configHash,exeHash);
        if(result&&!wasCached){fs.mkdirSync(path.join(candidateDir,c.id),{recursive:true});write(path.join(candidateDir,c.id,'result.json'),{...result,reused_initial:true});}
      }
      if(!result)result=await runCase(baselineDir,baseline,c,candidateDir,configPath,digest({sig,params:candidate.params,id:c.id}));
      rows.push(result);console.log('['+candidate.id+'] '+c.id+' '+rows.length+'/'+cohort.length+' alert='+result.alert+' exp='+result.expected_alert);
      // Rest between videos: skip if noRest or reused (no actual inference)
      if(!wasCached&&!result.reused_initial&&!noRest&&rows.length<cohort.length)
        await rest(pacing.between_videos_ms,dir,{candidate:candidate.id,completed_cases:rows.length,total_cases:cohort.length});
    }
    const entry={...candidate,metrics:metrics50(rows),complete:rows.length===baseline.cases.length,smoke};
    entries.push(entry);write(path.join(candidateDir,'summary.json'),entry);
    // Update shared leaderboard (Node.js single-thread: safe, writes between awaits)
    const sortedEntries=[...entries].sort((a,b)=>a.id.localeCompare(b.id));
    write(path.join(dir,'leaderboard.json'),sortedEntries);
    const ranked=rank50(entries,constraints);
    if(ranked.length&&!smoke){
      write(path.join(dir,'best.json'),{...ranked[0],status:'best_so_far_on_development_data',applied_to_operational_config:false});
      write(path.join(dir,'best-config.json'),configFor(baseConfig,ranked[0].params));
    }
    console.log('['+candidate.id+'] done — primary F1='+(entry.metrics.primary.f1*100).toFixed(1)+'% FP='+entry.metrics.primary.FP);
  }

  if(parallelism<=1){
    // Sequential: include inter-candidate rests
    for(const candidate of allCandidates){
      await runCandidate(candidate);
      if(!noRest&&entries.length<allCandidates.length)await rest(pacing.between_candidates_ms,dir,{completed_candidates:entries.length,total_candidates:allCandidates.length});
    }
  } else {
    // Parallel pool: distribute candidates across N workers (round-robin by index)
    // No inter-candidate rests in parallel mode (candidates overlap in time)
    const n=Math.min(parallelism,allCandidates.length);
    await Promise.all(Array.from({length:n},(_,w)=>
      (async()=>{for(let i=w;i<allCandidates.length;i+=n)await runCandidate(allCandidates[i]);})()
    ));
  }

  if(smoke){
    const firstCase=cohort[0],freshResult=read(path.join(dir,'candidate-00',firstCase.id,'result.json'));
    if(Math.abs(freshResult.prediction_s-firstCase.reference_s)>0.251)throw Error('Smoke: checkpoint not reached for '+firstCase.id);
    write(path.join(dir,'smoke-result.json'),{passed:true,id:firstCase.id,prediction_s:freshResult.prediction_s,reference_s:firstCase.reference_s,note:'Smoke: confirms one full evaluation run; not tuning.'});
  }
  const ranked=rank50(entries,constraints);
  const sortedAll=[...entries].sort((a,b)=>a.id.localeCompare(b.id));
  const mdRows=sortedAll.map(e=>{const m=e.metrics.primary;const pass=m.FP<=constraints.max_false_positives&&m.quality_coverage>=constraints.min_quality_coverage&&m.recall>=(constraints.min_recall??0);return`| ${e.id} | ${e.params.threshold} | ${e.params.min_area} | ${e.params.confirm_seconds} | ${m.TP} | ${m.FP} | ${m.TN} | ${m.FN} | ${(m.precision*100).toFixed(1)} | ${(m.recall*100).toFixed(1)} | ${(m.f1*100).toFixed(1)} | ${(m.accuracy*100).toFixed(1)} | ${pass?'통과':'탈락'} |`;});
  const md=`# 50-video Grid Search Results\n\nPrimary metrics exclude S33 (protocol-changed) and null-labeled cases.\n\n| 후보 | threshold | min_area | 확인초 | TP | FP | TN | FN | 정밀도 | 재현율 | F1 | 정확도 | 제약 |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|\n${mdRows.join('\n')}\n\n**추천**: ${ranked[0]?ranked[0].id+' ('+JSON.stringify(ranked[0].params)+')':'없음'}\n`;
  fs.writeFileSync(path.join(dir,'report.md'),md);
  write(path.join(dir,'completed.json'),{finished_at:new Date().toISOString(),smoke,noRest,parallelism,candidates:entries.length,best:smoke?null:ranked[0]||null,operational_config_modified:false,applied_to_operational_config:false});
  write(path.join(dir,'progress.json'),{state:'completed',updated_at:new Date().toISOString()});
  console.log('Complete: '+dir);if(ranked[0])console.log('Best: '+JSON.stringify(ranked[0].params));else console.log('No candidate passed constraints.');
  return dir;
}

async function main(){
  const args=process.argv.slice(2);const mode=args[0],arg=args[1];
  const projectRootFlag=args.indexOf('--project-root');const projectRoot=projectRootFlag>=0?path.resolve(args[projectRootFlag+1]):null;
  const idsFlag=args.indexOf('--ids');const filterIds=idsFlag>=0?new Set(args[idsFlag+1].split(',')):null;
  const noRest=args.includes('--no-rest');
  const parallelFlag=args.indexOf('--parallel');const parallelism=parallelFlag>=0?Math.max(1,parseInt(args[parallelFlag+1])||1):1;
  if(mode==='--init'&&arg)await initialize(path.resolve(arg),projectRoot);
  else if(mode==='--plan'&&arg)plan(path.resolve(arg));
  else if(mode==='--run'&&arg)await search(path.resolve(arg),{noRest,parallelism});
  else if(mode==='--smoke'&&arg)await search(path.resolve(arg),{smoke:true,filterIds,noRest});
  else if(mode==='--resume'&&arg){const run=path.resolve(arg),m=read(path.join(run,'search.json'));await search(m.baseline,{resumeDir:run,noRest,parallelism});}
  else console.log('node scripts/surface_grid_search_all50.js --init ALL50_DIR [--project-root PATH] | --plan BASELINE | --run BASELINE [--no-rest] [--parallel N] | --resume SEARCH [--no-rest] [--parallel N] | --smoke BASELINE [--ids S01,S02]');
}
if(require.main===module)(['--run','--resume','--smoke'].includes(process.argv[2])?exclusiveSearch(main):main()).catch(e=>{console.error(e.message||e);process.exitCode=1;});
module.exports={variants,metrics50,rank50,configFor,resolvePath,mergeCasesAndLabels,PROTOCOL_CHANGED_IDS,initialize,runCase};
