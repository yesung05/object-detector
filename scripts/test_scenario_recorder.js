const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const {scenariosFromMarkdown,phases}=require('./recording_scenarios');
const {createServer}=require('./serve_scenario_recorder');
const root=path.resolve(__dirname,'..');
const scenarios=scenariosFromMarkdown(fs.readFileSync(path.join(root,'docs/논문/50-scenarios-recording-plan.md'),'utf8'));
class Element{
 constructor(){this.value='';this.checked=false;this.disabled=false;this.children=[];this.textContent='';this.min='1';this.max='600';}
 append(...children){this.children.push(...children);}replaceChildren(){this.children=[];}
 querySelectorAll(selector){const all=this.children.flatMap(e=>e.children||[]).filter(e=>e.type==='checkbox');return selector==='input:checked'?all.filter(e=>e.checked):all;}
}
async function main(){
 assert.equal(scenarios.length,50);assert.equal(new Set(scenarios.map(s=>s.id)).size,50);
 for(const s of scenarios){assert(s.action.length>20);assert(!/참여자 2명|두 사람|두 명|바닥 ROI|벽 ROI/.test(s.action));}
 const t={baseline:15,action:45,observe:90,cleanup:20,after:20};assert.equal(phases(scenarios[0],t).reduce((a,b)=>a+b.seconds,0),190);assert.equal(phases(scenarios[32],t).length,7);assert.match(phases(scenarios[38],t)[2].text,/혼자 계속 앉아/);
 const server=createServer();await new Promise(r=>server.listen(0,'127.0.0.1',r));
 try{const base='http://127.0.0.1:'+server.address().port;assert.equal((await (await fetch(base+'/api/scenarios')).json()).length,50);assert.equal((await fetch(base+'/')).status,200);assert.equal((await fetch(base+'/recorder.js')).status,200);assert.equal((await fetch(base+'/.git/config')).status,404);assert.equal((await fetch(base+'/',{method:'POST'})).status,405);}finally{await new Promise(r=>server.close(r));}
 const html=fs.readFileSync(path.join(root,'dashboard/scenario-recorder.html'),'utf8');
 const ids=[...html.matchAll(/id="([^"]+)"/g)].map(m=>m[1]);assert.equal(new Set(ids).size,ids.length);
 const nodes=Object.fromEntries(ids.map(id=>[id,new Element()]));for(const [k,v]of Object.entries({...t,prepare:15,limit:1}))nodes[k].value=String(v);
 nodes.automatic.checked=false;let now=0,id=0,failVideo=false;const files=new Map(),events={};
 const dir={getDirectoryHandle:async()=>dir,getFileHandle:async name=>({createWritable:async()=>{const pieces=[];return {write:async data=>{if(failVideo&&name.endsWith('.webm'))throw Error('disk full');pieces.push(data);},close:async()=>files.set(name,pieces),abort:async()=>{}};}})};
 const track={readyState:'live',getSettings:()=>({width:1280,height:720,frameRate:30}),addEventListener(){},stop(){this.readyState='ended';}};
 const stream={getVideoTracks:()=>[track],getTracks:()=>[track]};
 class Recorder{static isTypeSupported(){return true;}constructor(){this.state='inactive';}start(){this.state='recording';this.onstart?.();this.ondataavailable({data:new Blob(['first'])});}stop(){this.state='inactive';this.ondataavailable({data:new Blob(['last'])});this.onstop?.();}}
 const sandbox={console,Blob,Date,Math,JSON,Error,Promise,MediaRecorder:Recorder,performance:{now:()=>now},crypto:{randomUUID:()=>String(++id).padStart(36,'0')},setInterval:()=>++id,clearInterval(){},navigator:{mediaDevices:{getUserMedia:async()=>stream,enumerateDevices:async()=>[]}},document:{hidden:false,getElementById:id=>nodes[id],createElement:()=>new Element(),createTextNode:text=>({textContent:text}),addEventListener:(n,f)=>events[n]=f},window:{recordingPhases:phases,showDirectoryPicker:async()=>dir,addEventListener:(n,f)=>events[n]=f},fetch:async()=>({ok:true,json:async()=>scenarios})};
 vm.createContext(sandbox);const run=code=>vm.runInContext(code,sandbox);const settle=async()=>{await new Promise(r=>setImmediate(r));};
 run(fs.readFileSync(path.join(root,'dashboard/browser-voice.js'),'utf8'));run(fs.readFileSync(path.join(root,'dashboard/scenario-recorder.js'),'utf8'));await settle();
 assert.equal(run('shuffle(scenarios).length'),50);assert.equal(run('new Set(shuffle(scenarios).map(s=>s.id)).size'),50);
 run("retakePlan={generated_at:'test',items:[{id:'S06',category:'retake'},{id:'S01',category:'missing'}]};selectRetakes(false)");assert.equal(nodes.limit.value,1);assert.equal(run("$('choices').querySelectorAll('input:checked')[0].value"),'S06');
 run('selectRetakes(true)');assert.equal(nodes.limit.value,2);
 nodes.all.onclick();nodes.limit.value='1';
 run("voicePlayer.play=()=>new Promise(resolve=>voicePlayer.cancelCurrent=resolve);$('voice').checked=true;window.pendingSpeech=say('test')");assert.equal(nodes['skip-explanation'].disabled,false);nodes['skip-explanation'].onclick();await run('window.pendingSpeech');assert.equal(nodes['skip-explanation'].disabled,true);assert.equal(run('promptSkips.length'),1);nodes.voice.checked=false;
 run("window.beepCount=0;audioContext={state:'running',currentTime:0,resume:async()=>{},destination:{},createOscillator:()=>({frequency:{},connect(){},start(){window.beepCount++},stop(){},disconnect(){}}),createGain:()=>({gain:{setValueAtTime(){},linearRampToValueAtTime(){}},connect(){},disconnect(){}})}");
 await nodes.camera.onclick();await nodes.folder.onclick();assert.equal(nodes.start.disabled,false);
 await run('start()');assert.equal(run('active'),true);await run('beginRecording()');await settle();
 const phaseCount=run('stageList.length');for(let i=0;i<phaseCount;i++){now+=100000;run('tick()');await settle();}
 assert.equal(run('window.beepCount'),1);assert.equal(run('session.results[0].start_beep'),'scheduled');
 assert.equal(run('active'),false,run('JSON.stringify({phaseIndex,deadline,stageList,stopReason,status:$("status").textContent})'));assert.equal(run('session.index'),1);assert.equal(run('session.results[0].status'),'completed');
 const video=[...files.keys()].find(k=>k.endsWith('.webm'));assert(video);assert.equal(files.get(video).length,2,'final MediaRecorder chunk saved');
 assert.equal(run('session.results[0].actual_actions_verified'),false);
 nodes.new.onclick();await run('start()');await run('beginRecording()');await settle();sandbox.document.hidden=true;events.visibilitychange();await settle();assert.equal(run('session.index'),0);assert.equal(run('session.results[0].status'),'partial');assert.equal(run('session.results[0].stop_reason'),'tab_hidden');sandbox.document.hidden=false;
 await run('start()');failVideo=true;await run('beginRecording()');await settle();assert.equal(run('active'),false);assert.equal(run('session.index'),0);assert.equal(run('session.results.at(-1).status'),'failed');
 console.log('Scenario recorder PASS: 50 one-person prompts, phases, shuffle, local HTTP, final chunk, partial save, disk failure. Synthetic camera only.');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
