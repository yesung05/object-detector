const assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {buildRetakePlan}=require('./recording_retake_plan');
const root=fs.mkdtempSync(path.join(os.tmpdir(),'retake-test-'));
const scenarios=['S01','S02','S03','S04','S05','S17','S38'].map(id=>({id}));
function session(name,results){const dir=path.join(root,name);fs.mkdirSync(dir);fs.writeFileSync(path.join(dir,'session.json'),JSON.stringify({results}));for(const r of results)if(r.filename&&!r.filename.includes('..'))fs.writeFileSync(path.join(dir,r.filename),'video');return dir;}
session('session-2026-09-11T07-12-10-284Z-7d0c50b3',[{scenario_id:'S01',status:'completed',filename:'good.webm',bytes_written:5},{scenario_id:'S02',status:'partial',filename:'partial.webm'},{scenario_id:'S03',status:'skipped'},{scenario_id:'S04',status:'completed',filename:'size.webm',bytes_written:99},{scenario_id:'S17',status:'completed',filename:'ambiguous.webm'},{scenario_id:'S38',status:'completed',filename:'../outside.webm'}]);
let p=buildRetakePlan(root,scenarios);const category=id=>p.items.find(x=>x.id===id).category;
assert.equal(category('S01'),'available');assert.equal(category('S02'),'retake');assert.equal(category('S03'),'retake');assert.equal(category('S04'),'retake');assert.equal(category('S05'),'missing');assert.equal(category('S17'),'retake');assert.equal(category('S38'),'retake');
session('new-take',[{scenario_id:'S17',status:'completed',filename:'new.webm',bytes_written:5}]);
p=buildRetakePlan(root,scenarios);assert.equal(category('S17'),'available');
fs.unlinkSync(path.join(root,'new-take','new.webm'));p=buildRetakePlan(root,scenarios);assert.equal(category('S17'),'retake');
const sidecar=session('recovered',[]);fs.writeFileSync(path.join(sidecar,'clip.webm'),'video');fs.writeFileSync(path.join(sidecar,'clip.webm.json'),JSON.stringify({scenario_id:'S05',status:'completed',filename:'clip.webm',bytes_written:5}));
p=buildRetakePlan(root,scenarios);assert.equal(category('S05'),'available');
console.log('Retake plan PASS: missing, skipped, partial, size mismatch, unsafe path, reviewed exclusion, replacement, sidecar recovery. Fixtures: '+root);
