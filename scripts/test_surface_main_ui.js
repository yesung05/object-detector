const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const assert=require('node:assert/strict');
const html=fs.readFileSync(path.join(__dirname,'../dashboard/index.html'),'utf8');
const js=html.match(/<script id="surface-main-script">([\s\S]*?)<\/script>/)[1];
class Element{
  constructor(){this.children=[];this.style={};this.dataset={};this.value='8081';this.textContent='';}
  append(...children){this.children.push(...children);}
  replaceChildren(){this.children=[];this.textContent='';}
}
async function main(){
  const nodes=Object.fromEntries(['surface-main-health','surface-main-cards','surface-main-message','port-input'].map(id=>[id,new Element()]));
  let tick,now=10000,captures=0,fail=false;
  const s={id:'table-1',baseline:true,reference:'surface-test.bin',capturing:false,quality:0,age:0,occupancy:'vacant',candidates:[]};
  const data={enabled:true,revision:1,frame_time:1,surfaces:[s]};
  const sandbox={document:{getElementById:id=>nodes[id],createElement:()=>new Element()},location:{protocol:'http:',hostname:'localhost'},Date:{now:()=>now},AbortController,setTimeout,clearTimeout,confirm:()=>true,setInterval:fn=>tick=fn,
    fetch:async url=>{if(fail)throw Error('offline');return {ok:true,json:async()=>{if(url==='/api/surfaces')return {enabled:true,revision:1,surfaces:[s]};if(url.includes('/surface/capture')){captures++;return {};}return data;}};}};
  vm.runInNewContext(js,sandbox);await new Promise(r=>setTimeout(r,0));
  const card=()=>nodes['surface-main-cards'].children[0];
  const label=()=>card().children.find(v=>v.className==='surface-verdict').textContent;
  assert.equal(label(),'이상 없음');assert.equal(card().children[0].textContent,'테이블 1');
  const cases=[
    [{candidates:[{alert:true}]},'청소 확인 필요'],
    [{candidates:[{animal:true}],occupancy:'occupied'},'동물 확인 필요'],
    [{candidates:[],occupancy:'occupied'},'사람이 사용 중이에요'],
    [{occupancy:'vacant',quality:2},'가려져서 안 보여요'],
    [{quality:0,baseline:false},'처음 설정이 필요해요'],
    [{capturing:true,capture_count:3},'깨끗한 모습 기억 중'],
    [{capturing:false,baseline:true,candidates:[{alert:false}]},'확인 중'],
    [{candidates:[],age:9},'화면 확인 필요']
  ];
  for(const [changes,expected] of cases){Object.assign(s,changes);data.frame_time++;await tick();assert.equal(label(),expected);}
  s.age=0;data.frame_time++;await tick();now+=5000;await tick();assert.equal(label(),'화면 확인 필요');
  data.frame_time++;await tick();
  const controls=card().children.at(-1),button=controls.children.find(v=>v.className==='btn');
  await button.onclick();assert.equal(captures,1,'main dashboard supports confirmed baseline capture');
  // Let the non-blocking refresh triggered by capture finish.
  await new Promise(r=>setTimeout(r,0));fail=true;await tick();
  assert.equal(nodes['surface-main-cards'].children.length,0,'offline never keeps a green card');
  assert.match(nodes['surface-main-health'].textContent,/연결/);
  console.log('Main surface cards + capture: PASS');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
