/* Browser/OS Korean speech. No application API key or generated audio required. */
class BrowserVoice {
 constructor(){this.cancelCurrent=null;this.current=null;}
 async load(){const ready=!!(window.speechSynthesis&&window.SpeechSynthesisUtterance);return {ready};}
 cancel(){if(this.cancelCurrent)this.cancelCurrent();}
 play(text){
  this.cancel();
  if(!window.speechSynthesis||!window.SpeechSynthesisUtterance)return Promise.reject(Error('이 브라우저는 음성 안내를 지원하지 않습니다. 음성 안내를 끄거나 다른 브라우저를 사용하세요.'));
  return new Promise((resolve,reject)=>{
   const synth=window.speechSynthesis,u=new window.SpeechSynthesisUtterance(text);u.lang='ko-KR';u.rate=1;u.pitch=1;
   const korean=synth.getVoices().filter(v=>/^ko(?:-|_)/i.test(v.lang));u.voice=korean.find(v=>v.default)||korean[0]||null;
   let settled=false;this.current=u;
   const finish=error=>{if(settled)return;settled=true;clearTimeout(timer);u.onend=u.onerror=null;this.current=null;this.cancelCurrent=null;if(error)reject(error);else resolve();};
   const timer=setTimeout(()=>{finish(Error('음성 안내 응답이 없습니다. 미리듣기로 확인하거나 음성 안내를 꺼주세요.'));synth.cancel();},180000);
   this.cancelCurrent=()=>{finish();synth.cancel();};
   u.onend=()=>finish();u.onerror=()=>finish(Error('브라우저 음성 재생에 실패했습니다. 음성 미리듣기와 시스템 음성을 확인하세요.'));
   try{synth.speak(u);}catch(e){finish(Error('브라우저 음성을 시작하지 못했습니다.'));}
  });
 }
}
if(typeof window!=='undefined')window.BrowserVoice=BrowserVoice;
if(typeof module!=='undefined')module.exports={BrowserVoice};
