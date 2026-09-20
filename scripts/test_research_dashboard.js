const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync(path.join(__dirname,'../dashboard/research.html'),'utf8');
const js=html.match(/<script>([\s\S]*?)<\/script>/)[1];
class Element{
 constructor(){this.children=[];this.style={};this.dataset={};this.textContent='';this.value='8081';this.checked=false;this.disabled=false;this.classList={add(){},remove(){}};}
 append(...v){this.children.push(...v);}replaceChildren(){this.children=[];this.textContent='';}click(){return this.onclick?.();}
 getContext(){return new Proxy({}, {get:()=>()=>{}});}toBlob(fn){fn(new Blob(['png'],{type:'image/png'}));}
}
async function main(){
 const ids=[...html.matchAll(/id="([^"]+)"/g)].map(v=>v[1]);assert.equal(new Set(ids).size,ids.length,'unique element IDs');
 assert(!/HUNIK|logo\.png/i.test(html),'neutral page contains no product branding');
 const css=html.match(/<style>([\s\S]*?)<\/style>/)[1];
 assert(!/border(?:-left)?:\s*[1-9]/.test(css),'paper layout has no decorative borders');
 assert(!/border-radius:\s*[1-9]/.test(css),'paper layout has square corners');
 const nodes=Object.fromEntries(ids.map(id=>[id,new Element()]));
 let tick,clock=10000,fail=false,downloaded=null;
 const s={id:'private-company-name',quality:0,baseline:true,occupancy:'vacant',age:0,candidates:[]};
 const data={enabled:true,revision:1,frame_time:1,surfaces:[s],ai_scheduler:{requests:0,reuse_observations:0}};
 class FakeDate extends Date{constructor(...args){super(...(args.length?args:[clock]));}static now(){return clock;}}
 const sandbox={document:{getElementById:id=>nodes[id],createElement:()=>new Element(),body:new Element()},location:{protocol:'file:',hostname:''},window:{print(){}},Image:class{},Date:FakeDate,AbortController,Blob,URL:{createObjectURL:blob=>{downloaded=blob;return 'blob:test';},revokeObjectURL(){}},setTimeout,clearTimeout,setInterval:fn=>tick=fn,
 fetch:async(url,opts)=>{assert.match(url,/http:\/\/localhost:8081\/surface\/status/);assert(!opts.method||opts.method==='GET','read only dashboard');if(fail)throw Error('offline');return {ok:true,json:async()=>structuredClone(data)};}};
 vm.createContext(sandbox);vm.runInContext(js,sandbox);await new Promise(r=>setTimeout(r,0));
 const report=()=>JSON.parse(vm.runInContext('JSON.stringify(report())',sandbox));
 assert.equal(report().areas[0].label,'이상 없음');assert.equal(report().areas[0].name,'영역 1');
 assert.equal(nodes.requests.textContent,'0','measured zero is not missing');
 nodes.export.click();assert(!((await downloaded.text()).includes('private-company-name')),'export omits original identifiers');
 nodes.png.click();assert.equal(downloaded.type,'image/png');
 nodes.freeze.click();clock+=10000;await tick();assert.equal(report().frozen,true);assert.equal(report().areas[0].label,'이상 없음','frozen report retains original observation');assert.match(nodes.health.textContent,/고정/);
 nodes.freeze.click();await new Promise(r=>setTimeout(r,0));assert.equal(report().areas[0].label,'화면 확인 필요','stopped frames cannot look healthy after resume');
 data.frame_time++;s.quality=2;await tick();assert.equal(report().areas[0].label,'가려져서 안 보여요');
 s.quality=0;s.candidates=[{alert:true}];data.frame_time++;await tick();assert.equal(report().areas[0].label,'청소 확인 필요');
 delete data.ai_scheduler;data.frame_time++;await tick();assert.equal(nodes.requests.textContent,'—','unsupported metrics are not fabricated');
 fail=true;await tick();assert.equal(nodes.cards.children.length,0);assert.equal(nodes.png.disabled,true);assert.equal(nodes.areas.textContent,'—');
 console.log('Research dashboard: status / freeze / privacy / export / offline PASS');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
