/* Offline preparation only. The web server never calls the paid API. */
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {scenariosFromMarkdown,phases}=require('./recording_scenarios');
const root=path.resolve(__dirname,'..'),voiceDir=path.join(root,'runs/scenario-voice');
const names={baseline:'초기 장면',preplace:'사전 배치',preobserve:'사전 관찰',action:'행동하세요',observe:'관찰 중',cleanup:'정리하세요',after:'정리 후 관찰'};
function catalog(){const scenarios=scenariosFromMarkdown(fs.readFileSync(path.join(root,'docs/논문/50-scenarios-recording-plan.md'),'utf8')),texts=new Set(['전체 촬영이 완료되었습니다.']);for(const s of scenarios){texts.add(s.id+'. 준비하세요. '+s.props+'. '+s.action);for(const p of phases(s,{baseline:15,action:45,observe:90,cleanup:20,after:20}))texts.add(names[p.kind]+'. '+p.text);}return [...texts];}
function key(text){return crypto.createHash('sha256').update(JSON.stringify({provider:'clova',speaker:'nara',speed:0,format:'mp3',text})).digest('hex');}
function validMp3(buffer){return buffer.length>16&&(buffer.subarray(0,3).toString()==='ID3'||(buffer[0]===255&&(buffer[1]&224)===224));}
function cached(text,directory=voiceDir){const file=path.join(directory,key(text)+'.mp3');try{return validMp3(fs.readFileSync(file));}catch{return false;}}
function voiceManifest(directory=voiceDir){const texts=catalog(),clips={};for(const text of texts)if(cached(text,directory))clips[text]='/voice/'+key(text)+'.mp3';return {provider:'CLOVA Voice',speaker:'nara',speed:0,ready:Object.keys(clips).length===texts.length,total:texts.length,available:Object.keys(clips).length,clips};}
async function generate({directory=voiceDir,env=process.env,request=fetch,texts=catalog()}={}){
 const missing=texts.filter(text=>!cached(text,directory));if(!missing.length)return {generated:0,reused:texts.length};
 if(!env.CLOVA_CLIENT_ID||!env.CLOVA_CLIENT_SECRET)throw Error('CLOVA_CLIENT_ID and CLOVA_CLIENT_SECRET must be set in the server terminal. Do not paste secrets into chat or browser.');
 fs.mkdirSync(directory,{recursive:true});let generated=0;
 for(const text of missing){
  if(text.length>2000)throw Error('Prompt exceeds CLOVA Korean text limit');
  const response=await request('https://naveropenapi.apigw.ntruss.com/tts-premium/v1/tts',{method:'POST',headers:{'X-NCP-APIGW-API-KEY-ID':env.CLOVA_CLIENT_ID,'X-NCP-APIGW-API-KEY':env.CLOVA_CLIENT_SECRET,'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({speaker:'nara',volume:'0',speed:'0',pitch:'0',format:'mp3',text}),signal:AbortSignal.timeout(30000)});
  if(!response.ok)throw Error('CLOVA request failed: HTTP '+response.status+' (response body hidden)');
  const buffer=Buffer.from(await response.arrayBuffer());if(buffer.length>5*1024*1024||!validMp3(buffer))throw Error('CLOVA returned invalid or oversized MP3');
  const target=path.join(directory,key(text)+'.mp3'),temp=target+'.'+crypto.randomUUID()+'.tmp';fs.writeFileSync(temp,buffer,{flag:'wx'});fs.renameSync(temp,target);generated++;
  console.log(`Voice prepared: ${generated}/${missing.length}`);
 }
 return {generated,reused:texts.length-missing.length};
}
if(require.main===module){const texts=catalog(),missing=texts.filter(t=>!cached(t));console.log(`CLOVA nara: ${texts.length} unique prompts, ${missing.length} missing, ${missing.reduce((n,t)=>n+t.length,0)} characters to synthesize. Charges depend on your plan.`);if(process.argv.includes('--generate'))generate().then(r=>console.log('Prepared:',r)).catch(e=>{console.error(e.message);process.exitCode=1;});else console.log('Preview only. Set credentials locally, then run with --generate to call the API.');}
module.exports={catalog,key,cached,validMp3,voiceManifest,generate,voiceDir};
