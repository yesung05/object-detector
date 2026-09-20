/* Read-only metadata/file audit, not semantic or decoder validation. */
const fs=require('node:fs'),path=require('node:path');
const reviewedSession='session-2026-09-11T07-12-10-284Z-7d0c50b3';
const reviewReasons={S17:'기존 육안 검토: 컵 잔류 여부 불명확',S38:'기존 육안 검토: 초기 장면에 물체가 있어 기준 불명확'};
function buildRetakePlan(recordsRoot,scenarios){
 const attempts=new Map(scenarios.map(s=>[s.id,[]])),warnings=[];
 for(const dir of fs.existsSync(recordsRoot)?fs.readdirSync(recordsRoot,{withFileTypes:true}):[]){
  if(!dir.isDirectory()||dir.isSymbolicLink())continue;
  const base=path.join(recordsRoot,dir.name),manifest=path.join(base,'session.json');
  let data={results:[]};
  if(!fs.existsSync(manifest))warnings.push(dir.name+': session.json 없음; 개별 영상 메타데이터만 확인');
  else try{data=JSON.parse(fs.readFileSync(manifest,'utf8'));if(!Array.isArray(data.results))throw Error('results 배열 없음');}catch(e){warnings.push(dir.name+': 세션 기록 읽기 실패; 개별 영상 메타데이터만 확인');data={results:[]};}
  const results=[...data.results];
  // Recover sidecars left behind if session.json saving was interrupted.
  for(const name of fs.readdirSync(base).filter(n=>n.endsWith('.webm.json'))){
   try{const r=JSON.parse(fs.readFileSync(path.join(base,name),'utf8'));if(!results.some(x=>x.filename===r.filename))results.push(r);}catch{warnings.push(dir.name+'/'+name+': 메타데이터 확인 필요');}
  }
  for(const r of results){
   const id=r.scenario?.id||r.scenario_id;if(!attempts.has(id))continue;
   let reason='',valid=false;
   if(r.status==='skipped')reason='건너뜀';
   else if(!r.filename||path.basename(r.filename)!==r.filename)reason='영상 파일명 누락/잘못됨';
   else{
    try{const stat=fs.lstatSync(path.join(base,r.filename));if(!stat.isFile()||stat.isSymbolicLink()||!stat.size)reason='빈 영상 또는 일반 파일 아님';else if(Number.isFinite(r.bytes_written)&&r.bytes_written!==stat.size)reason='기록된 용량과 실제 영상 용량 불일치';}catch{reason='영상 파일 없음';}
    if(!reason&&r.status!=='completed')reason=r.status==='partial'?'부분 녹화 (S06은 기존 체크포인트 평가에는 사용 가능)':'녹화 미완료/실패';
    if(!reason&&dir.name===reviewedSession&&reviewReasons[id])reason=reviewReasons[id];
    valid=!reason;
   }
   attempts.get(id).push({session:dir.name,filename:r.filename||null,status:r.status,valid,reason});
  }
 }
 return {generated_at:new Date().toISOString(),scope:'project records immediate session folders',warnings,items:scenarios.map(s=>{
  const a=attempts.get(s.id),usable=a.some(x=>x.valid);
  return {id:s.id,title:s.title,category:usable?'available':a.length?'retake':'missing',reason:usable?'완료 파일 있음 (행동 정확성 자동 검증 아님)':a.length?[...new Set(a.map(x=>x.reason))].join(' / '):'아직 촬영 기록 없음',attempts:a};
 })};
}
module.exports={buildRetakePlan};
