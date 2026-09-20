/* B comparison: immutable rebuilt evaluator + initial models, 50 original videos.
 * Does not start the grid automatically; creates a ready-to-review B snapshot. */
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {runCase,metrics50,initialize}=require('./surface_grid_search_all50');
const root=path.resolve(__dirname,'..'),read=p=>JSON.parse(fs.readFileSync(p,'utf8'));
const write=(p,v)=>fs.writeFileSync(p,JSON.stringify(v,null,2));
async function hash(p){const h=crypto.createHash('sha256');for await(const b of fs.createReadStream(p))h.update(b);return h.digest('hex');}
async function main(){
 const source=path.join(root,'runs/all50-initial-RQ5cAO'),old=read(path.join(source,'manifest.json')),labels=read(path.join(source,'labels.json'));
 if(old.cases.length!==50||labels.length!==50||labels.some(x=>typeof x.expected_alert!=='boolean'))throw Error('Expected 50 reviewed labels');
 const out=fs.mkdtempSync(path.join(root,'runs/chair-B-'));console.log('CHAIR_BASELINE_OUTPUT '+out);
 const config=read(path.join(root,'config/video-evaluation/all50-chair-boundary.json'));write(path.join(out,'config.json'),config);write(path.join(out,'labels.json'),labels);
 fs.mkdirSync(path.join(out,'evaluator'));fs.mkdirSync(path.join(out,'models'));fs.mkdirSync(path.join(out,'source'));
 for(const name of ['evaluate_surface_video.exe','onnxruntime.dll','onnxruntime_providers_shared.dll'])fs.copyFileSync(path.join(root,'build-windows/Release',name),path.join(out,'evaluator',name));
 const exe=path.join(out,'evaluator/evaluate_surface_video.exe'),models=[];
 for(const file of old.models){const dest=path.join(out,'models',path.basename(file));fs.copyFileSync(file,dest);models.push(dest);}
 for(const file of ['src/surface_monitor.c','src/surface_config.c','include/surface_monitor.h','tests/evaluate_surface_video.c','tests/test_surface.c','scripts/run_chair_baseline.js','scripts/surface_grid_search_all50.js'])fs.copyFileSync(path.join(root,file),path.join(out,'source',path.basename(file)));
 const artifacts=[];for(const file of [exe,...models,path.join(out,'evaluator/onnxruntime.dll'),path.join(out,'evaluator/onnxruntime_providers_shared.dll')])artifacts.push({file,sha256:await hash(file)});
 const cases=old.cases.map(c=>({...c,expected_alert:labels.find(l=>l.id===c.id).expected_alert}));
 const originalConfig=read(path.join(source,'config.json'));if(config.surfaces[0].fixtures.length!==originalConfig.surfaces[0].fixtures.length)throw Error('Fixture control changed');
 for(const c of cases)if(await hash(c.video)!==c.sha256)throw Error('Video mismatch '+c.id);
 const ffmpeg='C:/dev/ffmpeg-master-latest-win64-gpl-shared/bin/ffmpeg.exe',manifest={...old,created_at:new Date().toISOString(),scope:'B: dark-chair boundary handling only, initial parameters; development checkpoints',exe,models,artifacts,cases,initial_config_sha256:await hash(path.join(out,'config.json')),parallelism:4,comparison_source:source};write(path.join(out,'manifest.json'),manifest);
 const baseline={exe,models,ffmpeg},results=[],active={};let next=0;
 function progress(){write(path.join(out,'progress.json'),{state:results.length===50?'completed':'running',completed:results.length,total:50,active,at:new Date().toISOString()});}
 const signature=crypto.createHash('sha256').update(JSON.stringify(manifest)).digest('hex');
 await Promise.all(Array.from({length:4},(_,worker)=>(async()=>{for(;;){const i=next++;if(i>=cases.length)return;const c=cases[i];active[worker]=c.id;progress();const r=await runCase(out,baseline,c,out,path.join(out,'config.json'),signature);results.push(r);delete active[worker];progress();console.log(c.id+' '+results.length+'/50 alert='+r.alert+' exp='+r.expected_alert);}})()));
 results.sort((a,b)=>a.id.localeCompare(b.id));write(path.join(out,'results.json'),results);
 const initial=metrics50(read(path.join(source,'results.json'))),modified=metrics50(results);write(path.join(out,'comparison.json'),{A:initial,B:modified,changed:results.filter(r=>read(path.join(source,r.id,'result.json')).alert!==r.alert).map(r=>({id:r.id,expected:r.expected_alert,A:read(path.join(source,r.id,'result.json')).alert,B:r.alert})),note:'No ROI/fixture/threshold/model changes; chair unknown area measured separately.'});
 write(path.join(out,'completed.json'),{finished_at:new Date().toISOString(),primary:50,supplemental:0,operational_config_modified:false});
 const baseDir=await initialize(out,root),b=read(path.join(baseDir,'baseline.json'));
 b.selection_constraints={max_false_positives:Math.min(initial.primary.FP,modified.primary.FP),min_quality_coverage:Math.max(initial.primary.quality_coverage,modified.primary.quality_coverage),min_recall:Math.max(initial.primary.recall,modified.primary.recall)};
 b.selection_note='Compare identical 49-case denominator. Require no worse FP/recall/quality than both A and B checkpoints. Defined before grid candidate results; no forced recommendation.';
 write(path.join(baseDir,'baseline.json'),b);write(path.join(out,'grid-ready.json'),{baseline:baseDir,selection_constraints:b.selection_constraints});
 console.log('B_COMPARISON '+JSON.stringify({A:initial.primary,B:modified.primary}));console.log('GRID_BASELINE '+baseDir);
}
main().catch(e=>{console.error(e);process.exitCode=1;});
