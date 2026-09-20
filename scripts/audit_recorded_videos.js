/* Read originals only. Outputs inventory and review stills, not detector accuracy. */
const fs=require('node:fs'),path=require('node:path'),{execFileSync}=require('node:child_process');
const root=path.resolve(__dirname,'..'),sessionDir=path.resolve(root,process.argv[2]||'records/session-2026-09-11T07-12-10-284Z-7d0c50b3');
const ffroot='C:/dev/ffmpeg-master-latest-win64-gpl-shared/bin';
function run(name,args){return execFileSync(path.join(ffroot,name+'.exe'),args,{encoding:'utf8',windowsHide:true,maxBuffer:32*1024*1024,timeout:120000});}
const session=JSON.parse(fs.readFileSync(path.join(sessionDir,'session.json'),'utf8'));
const output=fs.mkdtempSync(path.join(root,'runs','record-audit-'));fs.mkdirSync(path.join(output,'images'));
const rows=[];
for(const result of session.results){
 if(!result.filename)continue;
 const video=path.join(sessionDir,path.basename(result.filename)),sidecar=JSON.parse(fs.readFileSync(video+'.json','utf8'));
 const row={id:result.scenario.id,title:result.scenario.title,status:result.status,filename:result.filename,bytes:fs.statSync(video).size,logged_duration_s:result.observed_duration_s,metadata_matches:JSON.stringify(sidecar)===JSON.stringify(result),probe:null,review_frames:[],warnings:[]};
 try{row.probe=JSON.parse(run('ffprobe',['-v','error','-show_entries','stream=codec_name,width,height,r_frame_rate,avg_frame_rate,duration:format=duration,size','-of','json',video]));}catch{row.warnings.push('probe failed');}
 const at=kind=>result.prompts.find(p=>p.kind===kind)?.countdown_started_elapsed_s;
 const samples=[['baseline',20],['action',(at('action')||40)+15],['early_observe',(at('observe')||95)+10],['late_observe',(at('observe')||95)+75],['end',result.observed_duration_s-3]];
 for(const [label,time]of samples){if(time>=result.observed_duration_s)continue;const file=`images/${row.id}-${label}.jpg`;
  try{run('ffmpeg',['-hide_banner','-loglevel','error','-threads','1','-ss',String(time),'-i',video,'-frames:v','1','-vf','scale=640:360','-q:v','3','-n',path.join(output,file)]);if(!fs.existsSync(path.join(output,file)))throw Error('no frame');row.review_frames.push({label,requested_s:time,file});}catch{row.warnings.push('frame extraction failed: '+label);}
 }
 rows.push(row);console.log(`${row.id} ${row.status}: ${row.review_frames.length} frames`);
 fs.writeFileSync(path.join(output,'audit.json'),JSON.stringify({source:sessionDir,generated_at:new Date().toISOString(),results:rows,skipped:session.results.filter(r=>r.status==='skipped'),accuracy_evaluated:false},null,2));
}
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
fs.writeFileSync(path.join(output,'review.html'),`<!doctype html><meta charset="utf-8"><title>촬영 자료 예비 검토</title><style>body{font:16px system-ui;background:#eef2f6}section{background:white;margin:20px;padding:16px}.frames{display:flex;flex-wrap:wrap}figure{margin:5px}img{width:480px}h1,p{margin:20px}</style><h1>촬영 자료 예비 검토 · 정답/감지 성능 미확정</h1><p>원본의 지정 시점 정지 화면입니다. 안내 시각은 실제 행동 시각이 아닙니다. 짧은 사건과 가림은 원본을 추가 확인해야 합니다.</p>${rows.map(r=>`<section><h2>${r.id} ${esc(r.title)} · ${r.status}</h2><p>${r.logged_duration_s.toFixed(2)}초 · ${esc(r.filename)}</p><div class="frames">${r.review_frames.map(f=>`<figure><img loading="lazy" src="${f.file}"><figcaption>${f.label} · 요청 ${f.requested_s.toFixed(2)}초</figcaption></figure>`).join('')}</div></section>`).join('')}`);
console.log('Saved: '+output);
