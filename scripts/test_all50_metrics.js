const assert=require('node:assert/strict'),{metrics}=require('./summarize_all50_initial');
const rows=[{expected_alert:true,alert:true,valid:true},{expected_alert:true,alert:false,valid:false},{expected_alert:false,alert:true,valid:true},{expected_alert:false,alert:false,valid:true}];
assert.deepEqual(metrics(rows),{n:4,TP:1,FP:1,TN:1,FN:1,precision:.5,recall:.5,f1:.5,accuracy:.5,quality_coverage:.75});
assert.throws(()=>metrics([{expected_alert:null,alert:false}]));assert.equal(metrics([]).accuracy,null);assert.equal(metrics([{expected_alert:false,alert:false,valid:true}]).TN,1);
console.log('All50 metrics PASS: confusion matrix, invalid quality retained, unknown label rejection, empty set.');
