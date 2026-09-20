/* Playback of pre-generated local files; no API keys or cloud requests. */
class ClovaPlayer {
 constructor(){this.clips={};this.current=null;this.cancelCurrent=null;}
 async load(){const r=await fetch('/api/voice');if(!r.ok)throw Error('음성 목록을 가져오지 못했습니다.');const m=await r.json();this.clips=m.clips||{};return m;}
 has(text){return typeof this.clips[text]==='string';}
 cancel(){if(this.cancelCurrent)this.cancelCurrent();}
 play(text){this.cancel();if(!this.has(text))return Promise.reject(Error('CLOVA 안내 파일이 없습니다. 먼저 음성을 생성해 주세요.'));
  return new Promise((resolve,reject)=>{const audio=new Audio(this.clips[text]);this.current=audio;let settled=false;const timer=setTimeout(()=>done(Error('안내 음성 재생 시간이 초과되었습니다.')),180000);
   const done=error=>{if(settled)return;settled=true;clearTimeout(timer);audio.pause();audio.onended=audio.onerror=null;this.current=null;this.cancelCurrent=null;if(error)reject(error);else resolve();};
   this.cancelCurrent=()=>done();audio.onended=()=>done();audio.onerror=()=>done(Error('CLOVA 음성을 재생하지 못했습니다. 파일과 스피커를 확인하세요.'));audio.play().catch(()=>done(Error('브라우저가 음성 재생을 차단했습니다. 음성 미리듣기를 누른 뒤 다시 시작하세요.')));
  });
 }
}
if(typeof window!=='undefined')window.ClovaPlayer=ClovaPlayer;
if(typeof module!=='undefined')module.exports={ClovaPlayer};
