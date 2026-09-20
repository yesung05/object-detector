/* Local evidence only: state transitions are predictions, never ground truth. */
const fs = require('node:fs');
const path = require('node:path');
function stateOf(row) {
 const s=row.surface, c=s.candidates||[];
 return {enabled:row.enabled, revision:row.revision, stale:row.stale, baseline:s.baseline,
  capturing:!!s.capturing, quality:s.quality, occupancy:s.occupancy,
  changed:c.length>0, alert:c.some(x=>x.alert), animal:c.some(x=>x.animal)};
}
const escapeHTML=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function createEvidence(directory,opts,elapsed) {
 let previous=null,lastCapture=-Infinity,index=0;
 const events=[];
 fs.mkdirSync(path.join(directory,'images'));
 return {
  async observe(row) {
   const state=stateOf(row), changes=previous?Object.keys(state).filter(k=>state[k]!==previous[k]):['initial'];
   previous=state;
   const periodic=elapsed()-lastCapture>=5;
   if(!changes.length&&!periodic)return;
   const event={observed_s:row.received_elapsed_s,changes:changes.length?changes:['periodic'],state,image:null};
   // Limit image requests during noisy transitions, but retain every transition log.
   if(elapsed()-lastCapture>=2) {
    lastCapture=elapsed();event.capture_requested_s=elapsed();
    try {
     const response=await fetch(opts.url+'/snapshot',{signal:AbortSignal.timeout(1500)});
     if(!response.ok)throw Error('HTTP '+response.status);
     const buffer=Buffer.from(await response.arrayBuffer());
     if(buffer.length>10*1024*1024||buffer.length<4||buffer[0]!==255||buffer[1]!==216||buffer[buffer.length-2]!==255||buffer[buffer.length-1]!==217)throw Error('Invalid/oversized JPEG');
     event.image=`images/${String(++index).padStart(5,'0')}.jpg`;
     fs.writeFileSync(path.join(directory,event.image),buffer);
     event.capture_received_s=elapsed();
    } catch(e) {event.capture_error=e.message;}
   } else event.capture_skipped='2 second rate limit';
   events.push(event);fs.appendFileSync(path.join(directory,'events.jsonl'),JSON.stringify(event)+'\n');
  },
  finish() {
   const names={initial:'기록 시작',periodic:'정기 화면',enabled:'감시 설정',revision:'설정 변경',stale:'영상 갱신 상태',baseline:'기준 모습',capturing:'기준 등록',quality:'가림·화면 상태',occupancy:'사람 이용 상태',changed:'변화 후보 유무',alert:'청소 알림',animal:'동물 후보'};
   const cards=events.map(e=>`<article><h2>${e.observed_s.toFixed(2)}초 · ${e.changes.map(k=>names[k]||k).join(', ')}</h2><p>${e.state.alert?'청소 알림 있음':'청소 알림 없음'} / 이용 상태: ${escapeHTML(e.state.occupancy)} / 화면 상태: ${escapeHTML(e.state.quality)}</p>${e.image?`<a href="${e.image}"><img loading="lazy" src="${e.image}" alt="관측 화면"></a><p>화면 요청 ${e.capture_requested_s}초 → 수신 ${e.capture_received_s}초</p>`:`<p>화면 미저장: ${escapeHTML(e.capture_error||e.capture_skipped)}</p>`}</article>`).join('\n');
   fs.writeFileSync(path.join(directory,'review.html'),`<!doctype html><html lang="ko"><meta charset="utf-8"><title>실험 자동 기록</title><style>body{font:16px/1.6 system-ui;background:#eef2f5;margin:24px}article{background:white;padding:20px;margin:16px 0}img{max-width:100%;max-height:540px}h2{font-size:19px}</style><h1>실험 자동 기록</h1><p>상태 변화 및 약 5초 간격 화면입니다. 동영상이 아니며 감지하지 못한 짧은 사건은 누락될 수 있습니다. 화면과 상태는 별도 HTTP 요청으로 정확히 같은 시점이 아닙니다. 알림 없음은 쓰레기 없음의 정답이 아닙니다.</p><p>이용 상태 vacant = 사람 없음, occupied = 이용 중, departure_pending = 퇴석 확인 중. 화면 상태 0 = 유효, 2 = 가림. 전체 원자료: <a href="samples.jsonl">상태 로그</a> / <a href="events.jsonl">화면 기록</a> / <a href="summary.json">요약</a></p>${cards}</html>`);
   return {events:events.length,images:index,review:'review.html',note:'Asynchronous JPEG evidence, not continuous video or ground truth.'};
  }
 };
}
module.exports={createEvidence,stateOf};
