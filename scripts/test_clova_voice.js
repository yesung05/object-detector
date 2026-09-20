const assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {catalog,key,generate,voiceManifest}=require('./clova_voice');
const {ClovaPlayer}=require('../dashboard/clova-player');
async function main(){
 const directory=fs.mkdtempSync(path.join(os.tmpdir(),'clova-test-')),text=catalog()[0];let calls=0;
 assert(catalog().length>100);assert.equal(key(text),key(text));assert.notEqual(key(text),key(text+'!'));
 await assert.rejects(generate({directory,env:{},texts:[text]}),/CLOVA_CLIENT_ID/);
 const request=async(url,options)=>{calls++;assert.equal(options.body.get('speaker'),'nara');assert.equal(options.body.get('text'),text);assert.equal(options.headers['X-NCP-APIGW-API-KEY'],'test-only');return {ok:true,arrayBuffer:async()=>Buffer.concat([Buffer.from('ID3'),Buffer.alloc(20)])};};
 await generate({directory,env:{CLOVA_CLIENT_ID:'test-only',CLOVA_CLIENT_SECRET:'test-only'},request,texts:[text]});
 await generate({directory,env:{},request,texts:[text]});assert.equal(calls,1);assert.equal(voiceManifest(directory).available,1);assert.equal(voiceManifest(directory).ready,false);
 await assert.rejects(generate({directory,env:{CLOVA_CLIENT_ID:'test',CLOVA_CLIENT_SECRET:'test'},request:async()=>({ok:false,status:401}),texts:['new']}),/HTTP 401/);
 const oldAudio=global.Audio,oldFetch=global.fetch;let audio;
 global.Audio=class{constructor(){audio=this;}pause(){}play(){return Promise.resolve();}};
 global.fetch=async()=>({ok:true,json:async()=>({clips:{hello:'/voice/test.mp3'}})});
 try{const player=new ClovaPlayer();await player.load();let ended=false;const p=player.play('hello').then(()=>ended=true);await Promise.resolve();assert.equal(ended,false,'waits for audio end');audio.onended();await p;assert.equal(ended,true);const q=player.play('hello');player.cancel();await q;await assert.rejects(player.play('missing'),/CLOVA/);const bad=player.play('hello');audio.onerror();await assert.rejects(bad,/재생/);}finally{global.Audio=oldAudio;global.fetch=oldFetch;}
 console.log('CLOVA PASS: catalog, credential guard, cache reuse, HTTP failure, playback completion/cancel/error. No paid API calls.');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
