/* Frozen-baseline grid search. Never applies parameters to operational config.
 * --init | --plan BASELINE | --run BASELINE | --resume RUN | --smoke BASELINE
 * All 21 existing checkpoints are development data, not an independent test set. */
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),{spawn}=require('node:child_process');
const root=path.resolve(__dirname,'..'),searchRoot=path.join(root,'runs/surface-search');
const ffmpeg='C:/dev/ffmpeg-master-latest-win64-gpl-shared/bin/ffmpeg.exe';
const grid={threshold:[16,24,32],min_area:[0.001,0.003],confirm_seconds:[5,10,15]};
const constraints={max_false_positives:2,min_quality_coverage:0.95};
const pacing={between_videos_ms:10000,between_candidates_ms:60000};
function restMs(kind,smoke=false){return smoke?0:(kind==='video'?pacing.between_videos_ms:pacing.between_candidates_ms);}
async function rest(ms,dir,context){if(!ms)return;write(path.join(dir,'progress.json'),{state:'resting',...context,updated_at:new Date().toISOString(),next_start_after:new Date(Date.now()+ms).toISOString()});console.log('Rest: '+ms/1000+' seconds');await new Promise(r=>setTimeout(r,ms));}
async function exclusiveSearch(action){const server=require('node:net').createServer(socket=>socket.end());await new Promise((resolve,reject)=>{server.once('error',()=>reject(Error('Another search is running, or local lock port 8093 is unavailable.')));server.listen(8093,'127.0.0.1',resolve);});try{return await action();}finally{await new Promise(r=>server.close(r));}}
async function hash(file){const h=crypto.createHash('sha256');for await(const chunk of fs.createReadStream(file))h.update(chunk);return h.digest('hex');}
function digest(value){return crypto.createHash('sha256').update(JSON.stringify(value)).digest('hex');}
function read(file){return JSON.parse(fs.readFileSync(file,'utf8'));}
function write(file,value){const temp=file+'.'+crypto.randomUUID()+'.tmp';fs.writeFileSync(temp,JSON.stringify(value,null,2));fs.renameSync(temp,file);}
function variants(){const all=[];for(const threshold of grid.threshold)for(const min_area of grid.min_area)for(const confirm_seconds of grid.confirm_seconds)all.push({threshold,min_area,confirm_seconds});const initial={threshold:24,min_area:0.003,confirm_seconds:15};return [initial,...all.filter(p=>digest(p)!==digest(initial))].map((params,i)=>({id:'candidate-'+String(i).padStart(2,'0'),params}));}
function metrics(rows){const counts={TP:0,FP:0,TN:0,FN:0};for(const r of rows)counts[r.expected_residue?(r.predicted_alert?'TP':'FN'):(r.predicted_alert?'FP':'TN')]++;const {TP,FP,TN,FN}=counts,n=rows.length;return {...counts,n,precision:TP+FP?TP/(TP+FP):0,recall:TP+FN?TP/(TP+FN):0,f1:2*TP+FP+FN?2*TP/(2*TP+FP+FN):0,accuracy:n?(TP+TN)/n:0,quality_coverage:n?rows.filter(r=>r.valid_at_reference).length/n:0};}
function rank(entries,limits=constraints){return entries.filter(e=>e.complete&&e.metrics.FP<=limits.max_false_positives&&e.metrics.quality_coverage>=limits.min_quality_coverage).sort((a,b)=>b.metrics.f1-a.metrics.f1||a.metrics.FP-b.metrics.FP||b.metrics.quality_coverage-a.metrics.quality_coverage||a.id.localeCompare(b.id));}
function configFor(base,params){const config=structuredClone(base);const table=config.surfaces.find(s=>s.id==='table-1');if(!table)throw Error('table-1 missing');Object.assign(table,params);return config;}
async function initialize(){
 fs.mkdirSync(searchRoot,{recursive:true});const dir=fs.mkdtempSync(path.join(searchRoot,'baseline-'));
 const prior=path.join(root,'runs/video-evaluation-3yhvPj'),priorManifest=read(path.join(prior,'manifest.json')),priorResults=read(path.join(prior,'results.json'));
 if(priorResults.length!==21||new Set(priorResults.map(r=>r.id)).size!==21)throw Error('Expected frozen 21-case cohort');
 fs.cpSync(prior,path.join(dir,'initial-evaluation'),{recursive:true,errorOnExist:true,force:false});
 fs.mkdirSync(path.join(dir,'operational-config'));fs.copyFileSync(path.join(root,'config/surfaces.json'),path.join(dir,'operational-config/surfaces.json'));
 for(const name of fs.readdirSync(path.join(root,'config')))if(/^surface-.*-normal\.bin$/.test(name))fs.copyFileSync(path.join(root,'config',name),path.join(dir,'operational-config',name));
 fs.mkdirSync(path.join(dir,'models'));const models=[];for(const m of priorManifest.models){const source=path.join(root,m.file);if(await hash(source)!==m.sha256)throw Error('Model changed since initial evaluation: '+m.file);const target='models/'+path.basename(m.file);fs.copyFileSync(source,path.join(dir,target));models.push({file:target,sha256:m.sha256});}
 fs.mkdirSync(path.join(dir,'evaluator'));for(const name of ['evaluate_surface_video.exe','onnxruntime.dll','onnxruntime_providers_shared.dll'])fs.copyFileSync(path.join(root,'build-windows/surface-validation',name),path.join(dir,'evaluator',name));
 fs.mkdirSync(path.join(dir,'source'));for(const file of ['tests/evaluate_surface_video.c','src/surface_monitor.c','src/surface_config.c','src/detector_ort.c','src/postprocess.c','scripts/surface_grid_search.js'])fs.copyFileSync(path.join(root,file),path.join(dir,'source',path.basename(file)));
 const cohort=[];for(const r of priorResults){const video=path.join(priorManifest.source,r.filename||fs.readdirSync(priorManifest.source).find(n=>n.startsWith(r.id+'_')&&n.endsWith('.webm')));cohort.push({id:r.id,video,sha256:await hash(video),expected_residue:r.expected_residue,reference_s:r.reference_s,source_status:r.source_status});}
 const frozenFiles=[];async function inventory(folder){for(const entry of fs.readdirSync(folder,{withFileTypes:true})){const full=path.join(folder,entry.name);if(entry.isDirectory())await inventory(full);else frozenFiles.push({file:path.relative(dir,full).replace(/\\/g,'/'),sha256:await hash(full)});}}await inventory(dir);
 const manifest={schema_version:1,created_at:new Date().toISOString(),purpose:'development tuning, not independent test performance',initial_metrics:metrics(priorResults),cohort,excluded:priorManifest.excluded,models,ffmpeg,ffmpeg_sha256:await hash(ffmpeg),frozenFiles,grid,constraints};
 write(path.join(dir,'baseline.json'),manifest);console.log('Frozen baseline: '+dir);console.log(JSON.stringify(manifest.initial_metrics));return dir;
}
async function verify(dir,baseline){for(const f of baseline.frozenFiles)if(await hash(path.join(dir,f.file))!==f.sha256)throw Error('Frozen file changed: '+f.file);if(await hash(baseline.ffmpeg)!==baseline.ffmpeg_sha256)throw Error('FFmpeg binary changed');for(const c of baseline.cohort)if(await hash(c.video)!==c.sha256)throw Error('Input video changed: '+c.id);}
function plan(dir){const b=read(path.join(dir,'baseline.json'));const report={baseline:dir,cases:b.cohort.length,candidates:variants().length,cold_video_runs:variants().length*b.cohort.length,additional_video_runs:(variants().length-1)*b.cohort.length,grid,constraints,ranking:'F1 descending, then fewer FP, then quality coverage, then stable candidate ID',scope:'All cases were previously inspected; scores are development-set tuning scores. No holdout/generalization claim.',note:'Sequential full inference; potentially hours. Each case saved for resume. Operational files never changed.'};console.log(JSON.stringify(report,null,2));return report;}
async function runCase(baselineDir,baseline,row,candidateDir,configPath,signature){
 const work=path.join(candidateDir,row.id);fs.mkdirSync(work,{recursive:true});const resultPath=path.join(work,'result.json');
 if(fs.existsSync(resultPath)){const cached=read(resultPath);if(cached.signature===signature&&cached.predictions_sha256===await hash(path.join(work,'predictions.jsonl')))return cached;throw Error('Cached case changed: '+row.id);}
 // A failed previous attempt may have partial predictions/baselines. Use a fresh attempt folder.
 const scratch=fs.mkdtempSync(path.join(work,'attempt-')),prediction=path.join(work,'predictions.jsonl'),out=fs.createWriteStream(prediction),log=fs.createWriteStream(path.join(work,'process.log'));
 const ff=spawn(baseline.ffmpeg,['-hide_banner','-loglevel','error','-threads','1','-i',row.video,'-an','-vf','fps=fps=2:start_time=0','-pix_fmt','rgb24','-f','rawvideo','pipe:1'],{windowsHide:true,stdio:['ignore','pipe','pipe']});
 const detector=spawn(path.join(baselineDir,'evaluator/evaluate_surface_video.exe'),[configPath,scratch,...baseline.models.map(m=>path.join(baselineDir,m.file)),'1'],{windowsHide:true,stdio:['pipe','pipe','pipe']});
 ff.stdout.pipe(detector.stdin);detector.stdout.pipe(out);ff.stderr.pipe(log,{end:false});detector.stderr.pipe(log,{end:false});detector.stdin.on('error',()=>ff.kill());
 let stopped=false;const stop=()=>{stopped=true;ff.kill();detector.kill();};out.on('error',stop);log.on('error',stop);const timeout=setTimeout(stop,600000);process.once('SIGINT',stop);process.once('SIGTERM',stop);
 const wait=child=>new Promise((resolve,reject)=>{child.once('error',e=>{stop();reject(e);});child.once('close',code=>{if(code!==0)stop();resolve(code);});});
 try{const codes=await Promise.all([wait(ff),wait(detector)]);await new Promise((resolve,reject)=>{if(out.closed)return resolve();out.once('close',resolve);out.once('error',reject);});if(stopped||codes.some(c=>c!==0))throw Error('Evaluation interrupted/failed: '+row.id+'; see '+work);}
 finally{clearTimeout(timeout);process.removeListener('SIGINT',stop);process.removeListener('SIGTERM',stop);log.end();}
 const sequence=fs.readFileSync(prediction,'utf8').trim().split('\n').map(JSON.parse);if(!sequence.length||sequence.at(-1).video_s<row.reference_s-0.5)throw Error('Incomplete timeline: '+row.id);
 const chosen=sequence.reduce((a,b)=>Math.abs(a.video_s-row.reference_s)<Math.abs(b.video_s-row.reference_s)?a:b),surface=chosen.status.surfaces[0];if(Math.abs(chosen.video_s-row.reference_s)>.251)throw Error('Missing checkpoint: '+row.id);
 const result={signature,id:row.id,expected_residue:row.expected_residue,prediction_s:chosen.video_s,predicted_alert:surface.candidates.some(c=>c.alert),valid_at_reference:!!surface.baseline&&!surface.capturing&&surface.quality===0,quality:surface.quality,predictions_sha256:await hash(prediction)};write(resultPath,result);return result;
}
async function search(baselineDir,{resumeDir=null,smoke=false}={}){
 baselineDir=path.resolve(baselineDir);const baseline=read(path.join(baselineDir,'baseline.json'));await verify(baselineDir,baseline);
 const base=read(path.join(baselineDir,'initial-evaluation/config.json')),signature=digest({baseline,grid,constraints,pacing,version:1,runner_sha256:await hash(__filename)});
 const dir=resumeDir||fs.mkdtempSync(path.join(searchRoot,smoke?'smoke-':'search-'));const manifestPath=path.join(dir,'search.json');
 if(resumeDir){const old=read(manifestPath);if(old.signature!==signature)throw Error('Search definition/baseline changed; create a new search.');smoke=old.smoke;}else write(manifestPath,{baseline:baselineDir,signature,smoke,grid,constraints,pacing,created_at:new Date().toISOString()});
 const selected=smoke?[variants()[0]]:variants(),cohort=smoke?baseline.cohort.slice(0,1):baseline.cohort,entries=[];
 console.log('Search output: '+dir);
 for(const candidate of selected){const candidateDir=path.join(dir,candidate.id);fs.mkdirSync(candidateDir,{recursive:true});const config=configFor(base,candidate.params),configPath=path.join(candidateDir,'config.json');
  if(fs.existsSync(configPath)&&digest(read(configPath))!==digest(config))throw Error('Candidate config changed');write(configPath,config);
  const rows=[];
  if(!smoke&&candidate.id==='candidate-00')rows.push(...read(path.join(baselineDir,'initial-evaluation/results.json')));
  else for(const row of cohort){const wasCached=fs.existsSync(path.join(candidateDir,row.id,'result.json'));write(path.join(dir,'progress.json'),{state:'running',candidate:candidate.id,scenario:row.id,completed_cases:rows.length,total_cases:cohort.length,updated_at:new Date().toISOString()});const result=await runCase(baselineDir,baseline,row,candidateDir,configPath,digest({signature,params:candidate.params,id:row.id}));rows.push(result);console.log(candidate.id+' '+row.id+' '+rows.length+'/'+cohort.length);if(!wasCached&&rows.length<cohort.length)await rest(restMs('video',smoke),dir,{candidate:candidate.id,completed_cases:rows.length,total_cases:cohort.length});}
  const entry={...candidate,metrics:metrics(rows),complete:rows.length===baseline.cohort.length,reused_initial:candidate.id==='candidate-00'&&!smoke};entries.push(entry);write(path.join(candidateDir,'summary.json'),entry);write(path.join(dir,'leaderboard.json'),entries);
  const ranked=rank(entries);if(ranked.length&&!smoke){write(path.join(dir,'best.json'),{...ranked[0],status:'best_so_far_on_development_data',applied_to_operational_config:false});write(path.join(dir,'best-config.json'),configFor(base,ranked[0].params));}
  if(!entry.reused_initial&&entries.length<selected.length)await rest(restMs('candidate',smoke),dir,{candidate:candidate.id,completed_candidates:entries.length,total_candidates:selected.length});
 }
 if(smoke){const original=read(path.join(baselineDir,'initial-evaluation/results.json')).find(r=>r.id===cohort[0].id),replayed=read(path.join(dir,'candidate-00',cohort[0].id,'result.json'));if(original.predicted_alert!==replayed.predicted_alert||original.quality!==replayed.quality)throw Error('Initial replay mismatch');write(path.join(dir,'smoke-result.json'),{passed:true,id:cohort[0].id,note:'Only verifies one initial case replay; not tuning.'});}
 write(path.join(dir,'completed.json'),{finished_at:new Date().toISOString(),smoke,candidates:entries.length,best:smoke?null:rank(entries)[0]||null,operational_config_modified:false});write(path.join(dir,'progress.json'),{state:'completed',updated_at:new Date().toISOString()});console.log('Complete: '+dir);return dir;
}
async function main(){const [mode,arg]=process.argv.slice(2);if(mode==='--init')await initialize();else if(mode==='--plan'&&arg)plan(path.resolve(arg));else if(mode==='--run'&&arg)await search(arg);else if(mode==='--smoke'&&arg)await search(arg,{smoke:true});else if(mode==='--resume'&&arg){const run=path.resolve(arg),m=read(path.join(run,'search.json'));await search(m.baseline,{resumeDir:run});}else console.log('node scripts/surface_grid_search.js --init | --plan BASELINE | --run BASELINE | --resume RUN | --smoke BASELINE');}
if(require.main===module)(['--run','--resume','--smoke'].includes(process.argv[2])?exclusiveSearch(main):main()).catch(e=>{console.error(e.message);process.exitCode=1;});
module.exports={variants,metrics,rank,configFor,initialize,plan,search,restMs,exclusiveSearch};
