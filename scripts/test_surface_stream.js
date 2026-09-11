const fs=require('node:fs'),path=require('node:path'),os=require('node:os'),net=require('node:net');
const assert=require('node:assert/strict'),{spawn}=require('node:child_process');
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function main(){
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'hunik-surface-stream-'));
 fs.mkdirSync(path.join(root,'config'));
 const config={schema_version:1,revision:1,enabled:true,width:100,height:100,camera_id:'fixture',surfaces:[{
  id:'table-1',type:'table',polygon:[[.1,.1],[.9,.1],[.9,.9],[.1,.9]],usage_zones:[[[0,0],[.1,0],[.1,.1],[0,.1]]],
  enter_seconds:1,departure_seconds:2,confirm_seconds:2,clear_seconds:2}]};
 fs.writeFileSync(path.join(root,'config','surfaces.json'),JSON.stringify(config));
 const listener=net.createServer();await new Promise(r=>listener.listen(0,'127.0.0.1',r));
 const port=listener.address().port;await new Promise(r=>listener.close(r));
 const base=`http://127.0.0.1:${port}`;
 const child=spawn(process.env.SURFACE_TEST_STREAM || path.resolve(__dirname,'../build-windows/Release/test_surface_stream.exe'),[root,String(port)],{windowsHide:true,stdio:'ignore'});
 let launchError;child.on('error',e=>launchError=e);
 const poll=async predicate=>{for(let i=0;i<160;i++){if(launchError)throw launchError;try{const s=await(await fetch(base+'/surface/status')).json();if(predicate(s))return s;}catch{}await delay(100);}throw Error('stream state timeout');};
 try{
  const initial=await poll(s=>s.enabled);assert.equal(initial.surfaces[0].baseline,false);
  assert.equal((await fetch(base+'/surface/capture?id=table-1&empty=0',{method:'POST'})).status,202);
  const captured=await poll(s=>s.surfaces[0].baseline);
  const preview=await fetch(base+'/surface/reference?file='+captured.surfaces[0].reference);
  assert.equal(preview.status,200);assert.equal(preview.headers.get('content-type'),'image/jpeg');
  const bytes=new Uint8Array(await preview.arrayBuffer());assert.equal(bytes[0],255);assert.equal(bytes[1],216);
  assert.equal((await fetch(base+'/surface/reference?file=surface-..-escape.bin')).status,400);
  const alerted=await poll(s=>s.surfaces[0].candidates.some(c=>c.alert));
  const c=alerted.surfaces[0].candidates.find(c=>c.alert);
  assert.equal((await fetch(base+`/surface/ack?id=table-1&candidate=${c.id}&revision=1&version=${c.version}`,{method:'POST'})).status,202);
  const acknowledged=await poll(s=>s.surfaces[0].candidates.some(c=>c.acknowledged));
  assert(acknowledged.surfaces[0].candidates.some(c=>c.alert),'acknowledgement is not clearing');
  const evidence=await fetch(base+`/surface/reference?file=${c.evidence_file}`);
  assert.equal(evidence.status,200);
  console.log('Surface stream capture / preview / alert / acknowledgement: PASS');
  console.log('Isolated fixture:',root);
 }finally{child.kill();}
}
main().catch(e=>{console.error(e);process.exitCode=1;});
