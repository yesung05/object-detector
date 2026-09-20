/* Optional real Chromium test with a synthetic camera, never the user's camera. */
const fs=require('node:fs'),os=require('node:os'),path=require('node:path'),{spawn}=require('node:child_process'),assert=require('node:assert/strict');
const {createServer}=require('./serve_scenario_recorder');
async function main(){
 const server=createServer();await new Promise(r=>server.listen(0,'127.0.0.1',r));
 const profile=fs.mkdtempSync(path.join(os.tmpdir(),'scenario-browser-'));
 const child=spawn('C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',['--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check','--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream','--remote-debugging-port=0','--user-data-dir='+profile,'http://127.0.0.1:'+server.address().port],{windowsHide:true,stdio:'ignore'});
 let ws;const timeout=setTimeout(()=>child.kill(),30000);
 try{
  child.on('error',()=>{});const portfile=path.join(profile,'DevToolsActivePort');
  for(let i=0;i<80&&!fs.existsSync(portfile);i++)await new Promise(r=>setTimeout(r,100));
  if(!fs.existsSync(portfile))throw Error('Chromium did not start');
  const port=fs.readFileSync(portfile,'utf8').split('\n')[0];let page;
  for(let i=0;i<40;i++){const list=await(await fetch('http://127.0.0.1:'+port+'/json')).json();page=list.find(p=>p.type==='page');if(page)break;await new Promise(r=>setTimeout(r,100));}
  ws=new WebSocket(page.webSocketDebuggerUrl);await new Promise((r,j)=>{ws.onopen=r;ws.onerror=j;});
  let id=0;const pending=new Map();ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id){const cb=pending.get(m.id);pending.delete(m.id);cb?.(m);}};
  async function evaluate(expression){const n=++id;let timer;const p=new Promise((r,j)=>{pending.set(n,r);timer=setTimeout(()=>j(Error('Browser evaluation timeout: '+expression)),5000);});ws.send(JSON.stringify({id:n,method:'Runtime.evaluate',params:{expression,awaitPromise:true,returnByValue:true}}));let m;try{m=await p;}finally{clearTimeout(timer);pending.delete(n);}if(m.error||m.result.exceptionDetails)throw Error(JSON.stringify(m));return m.result.result.value;}
  for(let i=0;i<40;i++){if(await evaluate('typeof scenarios!=="undefined" && scenarios.length===50'))break;await new Promise(r=>setTimeout(r,100));}
  await evaluate(`window.testFiles=new Map();window.testDir={getDirectoryHandle:async()=>testDir,getFileHandle:async name=>({createWritable:async()=>{const parts=[];return {write:async x=>parts.push(x),close:async()=>testFiles.set(name,parts),abort:async()=>{}};}})};folder=testDir;$('limit').value='1';$('automatic').checked=false;$('voice').checked=false;connectCamera()`);
  assert.equal(await evaluate('healthy()'),true);
  await evaluate('start()');await evaluate('clearInterval(timer);beginRecording()');
  await new Promise(r=>setTimeout(r,1600));await evaluate('halt("completed")');
  for(let i=0;i<40;i++){if(await evaluate('!active'))break;await new Promise(r=>setTimeout(r,100));}
  assert.equal(await evaluate('session.results[0].status'),'completed');
  assert((await evaluate('session.results[0].bytes_written'))>1000);
  assert.equal(await evaluate(`(async()=>{const entry=[...testFiles].find(([n])=>n.endsWith('.webm'));const b=new Blob(entry[1]);const a=new Uint8Array(await b.slice(0,4).arrayBuffer());return [...a].map(n=>n.toString(16)).join('');})()`),'1a45dfa3');
  console.log('Chromium PASS: real MediaRecorder with fake camera produces WebM and completes metadata. No physical camera or disk video writer tested.');
 }finally{clearTimeout(timeout);ws?.close();child.kill();server.closeAllConnections();await new Promise(r=>server.close(r));}
}
main().catch(e=>{console.error(e);process.exitCode=1;});
