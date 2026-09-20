const assert=require('node:assert/strict');
const {BrowserVoice}=require('../dashboard/browser-voice');
async function main(){
 const previous=global.window;let utterance,canceled=0;
 global.window={SpeechSynthesisUtterance:class{constructor(text){this.text=text;}},speechSynthesis:{getVoices:()=>[{lang:'en-US'},{lang:'ko-KR',default:true}],speak:u=>utterance=u,cancel:()=>canceled++}};
 try{
  const player=new BrowserVoice();assert.equal((await player.load()).ready,true);
  let done=false;const p=player.play('행동하세요.').then(()=>done=true);await Promise.resolve();assert.equal(done,false);assert.equal(utterance.lang,'ko-KR');assert.equal(utterance.voice.lang,'ko-KR');utterance.onend();await p;assert.equal(done,true);
  const q=player.play('중단');player.cancel();await q;assert.equal(canceled,1);
  const r=player.play('오류');utterance.onerror();await assert.rejects(r,/실패/);
  delete window.speechSynthesis;assert.equal((await player.load()).ready,false);await assert.rejects(player.play('미지원'),/지원하지/);
 }finally{global.window=previous;}
 console.log('Browser voice PASS: Korean selection, wait for end, cancellation, error, unsupported browser. Synthetic speech events only.');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
