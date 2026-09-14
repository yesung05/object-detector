const assert=require('node:assert/strict');
const {variants,metrics50,rank50,configFor,resolvePath,mergeCasesAndLabels,PROTOCOL_CHANGED_IDS}=require('./surface_grid_search_all50');

// variants: same 18 combinations as 21-video version, same initial first
assert.equal(variants().length,18);
assert.equal(new Set(variants().map(c=>JSON.stringify(c.params))).size,18);
assert.deepEqual(variants()[0].params,{threshold:24,min_area:0.003,confirm_seconds:15});

// metrics50: uses expected_alert/alert/valid; excludes null and PROTOCOL_CHANGED_IDS
const rows=[
  {id:'S01',expected_alert:true, alert:true, valid:true},   // TP
  {id:'S02',expected_alert:true, alert:false,valid:true},   // FN
  {id:'S03',expected_alert:false,alert:true, valid:true},   // FP
  {id:'S04',expected_alert:false,alert:false,valid:true},   // TN
  {id:'S05',expected_alert:null, alert:true, valid:true},   // excluded (null label)
  {id:'S33',expected_alert:false,alert:true, valid:true},   // S33: in with_protocol_changed, not primary
];
const m=metrics50(rows);
assert.deepEqual(m.primary,{TP:1,FP:1,TN:1,FN:1,n:4,precision:.5,recall:.5,f1:.5,accuracy:.5,quality_coverage:1});
assert.equal(m.primary.n,4,'null and S33 excluded from primary');
assert.equal(m.with_protocol_changed.n,5,'S33 included in with_protocol_changed');
assert.equal(m.n_excluded_null,1);
assert.equal(m.n_protocol_changed,1);

// metrics50: quality_coverage uses valid field
const qrows=[
  {id:'A',expected_alert:true, alert:true, valid:true},
  {id:'B',expected_alert:false,alert:false,valid:false},  // invalid — counted in n but reduces coverage
];
const qm=metrics50(qrows);
assert.equal(qm.primary.quality_coverage,0.5);

// rank50: only passes candidates meeting FP and quality_coverage constraints
const constraints={max_false_positives:12,min_quality_coverage:0.96};
const entry=(id,f1,FP,quality_coverage=1,complete=true)=>({id,params:{threshold:24,min_area:0.003,confirm_seconds:15},metrics:{primary:{f1,FP,quality_coverage}},complete});
const ranking=rank50([entry('too_many_fp',.8,13),entry('low_quality',.8,10,.90),entry('incomplete',.9,5,1,false),entry('b',.7,5),entry('a',.7,3),entry('c',.6,0)],constraints);
assert.deepEqual(ranking.map(r=>r.id),['a','b','c'],'ranked by f1 then fp then quality');
assert.ok(!ranking.find(r=>r.id==='too_many_fp'),'FP>12 excluded');
assert.ok(!ranking.find(r=>r.id==='low_quality'),'quality<0.96 excluded');
assert.ok(!ranking.find(r=>r.id==='incomplete'),'incomplete excluded');

// configFor: only modifies table-1, leaves other surfaces intact, does not mutate original
const base={surfaces:[{id:'table-1',polygon:[[0,0],[1,0],[1,1]],departure_seconds:45},{id:'wall-1',threshold:10}]};
const before=JSON.stringify(base);
const changed=configFor(base,{threshold:32,min_area:0.001,confirm_seconds:10});
assert.equal(JSON.stringify(base),before,'original not mutated');
assert.equal(changed.surfaces[0].threshold,32);
assert.equal(changed.surfaces[0].departure_seconds,45,'unrelated field preserved');
assert.equal(changed.surfaces[1].threshold,10,'other surface unchanged');

// resolvePath: returns path as-is when it exists; migrates via anchor when it doesn't
const path=require('node:path'),os=require('node:os'),fs=require('node:fs');
const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'sgs-test-'));
fs.mkdirSync(path.join(tmp,'records','session-abc'),{recursive:true});
fs.writeFileSync(path.join(tmp,'records','session-abc','S01.webm'),'data');
// existing path: returned unchanged
const existing=path.join(tmp,'records','session-abc','S01.webm');
assert.equal(resolvePath(existing,null),existing);
// non-existent with migration
const oldPath='D:\\old-root\\records\\session-abc\\S01.webm';
assert.equal(path.normalize(resolvePath(oldPath,tmp)),path.normalize(existing));
// non-existent without project-root: throws
assert.throws(()=>resolvePath('D:\\nonexistent\\records\\x\\y.webm',null),/project-root/);
fs.rmSync(tmp,{recursive:true});

// mergeCasesAndLabels: labels.json values override manifest.cases expected_alert
const manifestCases=[
  {id:'S01',video:__filename,sha256:'x',reference_s:100,expected_alert:null,label_basis:'pending'},
  {id:'S02',video:__filename,sha256:'x',reference_s:100,expected_alert:null,label_basis:'pending'},
];
const labels=[
  {id:'S01',expected_alert:true, basis:'manual review',protocol_note:''},
  {id:'S02',expected_alert:false,basis:'manual review',protocol_note:'S33-like'},
];
const merged=mergeCasesAndLabels(manifestCases,labels,null);
assert.equal(merged[0].expected_alert,true);assert.equal(merged[0].label_basis,'manual review');
assert.equal(merged[1].expected_alert,false);assert.equal(merged[1].protocol_note,'S33-like');

// PROTOCOL_CHANGED_IDS contains S33
assert.ok(PROTOCOL_CHANGED_IDS.has('S33'));

console.log('Grid search all50 PASS: 18 variants, metrics50 field mapping, null/S33 exclusion, ranking, configFor, resolvePath migration, mergeCasesAndLabels.');
