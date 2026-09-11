/* Integration fixtures are isolated under the OS temp directory; never use
 * the live application's configuration or ports. No npm dependencies. */
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const net = require('node:net');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
const delay = ms => new Promise(r => setTimeout(r, ms));
async function freePort() {
  const server = net.createServer();
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const port = server.address().port;
  await new Promise(r => server.close(r));
  return port;
}
async function main() {
  const fixture = fs.mkdtempSync(path.join(os.tmpdir(), 'hunik-surface-http-'));
  fs.mkdirSync(path.join(fixture, 'dashboard'));
  fs.copyFileSync(path.join(root, 'dashboard', 'surfaces.html'), path.join(fixture, 'dashboard', 'surfaces.html'));
  const port = await freePort();
  const base = `http://127.0.0.1:${port}`;
  const child = spawn(process.env.SURFACE_TEST_DASHBOARD || path.join(root, 'build-windows', 'Release', 'hunik-dashboard.exe'),
    ['--root', fixture, '--port', String(port)], {windowsHide: true, stdio: 'ignore'});
  let launchError;
  child.on('error', e => { launchError = e; });
  try {
    let response;
    for(let i=0;i<50;i++) {
      if(launchError) throw launchError;
      try {response = await fetch(base+'/api/surfaces');break;}catch{await delay(100);}
    }
    assert(response, 'isolated dashboard starts');
    const initial = await response.json();
    assert.equal(initial.revision, 0);
    const polygon = [[.1,.1],[.9,.1],[.9,.9],[.1,.9]];
    const table = {id:'table-1',type:'table',polygon,usage_zones:[[[0,0],[.1,0],[.1,.1],[0,.1]]],locked:true,fixtures:[]};
    const config = {...initial,revision:1,enabled:true,surfaces:[table]};
    const post = value => fetch(base+'/api/surfaces',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(value)});
    fs.writeFileSync(path.join(fixture,'input.json'),JSON.stringify(config));
    const firstSave=await post(config);assert.equal(firstSave.status, 200,(await firstSave.text())+' fixture='+fixture);
    assert.equal((await post({...config,revision:2,ai_reuse:'invalid'})).status,400,'invalid scheduler flag rejected');
    assert.equal((await post(config)).status, 409, 'stale writes conflict');
    assert.equal((await post({...config,revision:2,source:'auto',surfaces:[{...table,threshold:90}]})).status,409,'auto proposal respects manual lock');
    assert.equal((await post({...config,revision:2,surfaces:[{...table,polygon:[[0,0],[1,1],[0,1],[1,0]]}]})).status,400,'self-intersecting polygon rejected');
    assert.equal((await post({...config,revision:2,surfaces:[table,{...table,id:'table-2'}]})).status,400,'overlap rejected');
    assert.equal((await post({...config,revision:2,surfaces:[{...table,usage_zones:[]}]})).status,400,'table needs usage zone');
    assert.equal((await post({...config,revision:2,surfaces:[{...table,id:'../escape'}]})).status,400,'unsafe ID rejected');
    const fixtureShape={id:'cup-1',polygon:[[.2,.2],[.3,.2],[.3,.3],[.2,.3]],movable:false};
    const badFixture=async(f,pattern)=>{const response=await post({...config,revision:2,surfaces:[{...table,fixtures:[f]}]});assert.equal(response.status,400);assert.match(await response.text(),pattern);};
    await badFixture({...fixtureShape,polygon:[[0,0],[.3,0],[.3,.3],[0,.3]]},/outside surface ROI.*surface 1, fixture 1/);
    await badFixture({...fixtureShape,movable:true},/allowed required/);
    await badFixture({...fixtureShape,allowed:[]},/fixtures.allowed/);
    await badFixture({...fixtureShape,polygon:[[.2,.2],[.3,.3],[.2,.3],[.3,.2]]},/fixtures.polygon/);
    assert.equal((await (await fetch(base+'/api/surfaces')).json()).revision,1,'invalid proposals preserve active file');
    /* Fragment the HTTP header/body deliberately. */
    const body=JSON.stringify({...config,revision:2});
    const raw=await new Promise((resolve,reject)=>{
      const socket=net.connect(port,'127.0.0.1');let result='';
      socket.on('error',reject);socket.on('data',b=>result+=b);socket.on('end',()=>resolve(result));
      socket.on('connect',async()=>{socket.write('POST /api/surfaces HTTP/1.1\r\nHo');await delay(30);
        socket.write(`st: 127.0.0.1\r\nContent-Length: ${Buffer.byteLength(body)}\r\nConnection: close\r\n\r\n`+body.slice(0,20));
        await delay(30);socket.write(body.slice(20));});
    });
    assert.match(raw,/HTTP\/1.1 200/,'fragmented HTTP request succeeds');
    assert.equal((await (await fetch(base+'/api/surfaces')).json()).revision,2);
    assert(fs.existsSync(path.join(fixture,'config','surfaces.json.revision-1')),'old revision retained');
    const html=await (await fetch(base+'/surfaces')).text();
    assert.match(html,/surface\/capture/);
    const js=html.match(/<script>([\s\S]*?)<\/script>/)[1];
    new vm.Script(js);
    /* Exercise UI event wiring and serialized draft without a browser dependency. */
    class Element {
      constructor(tag='div'){this.tag=tag;this.value='';this.checked=false;this.children=[];this.style={};this.width=1280;this.height=720;}
      append(...v){this.children.push(...v);if(this.tag==='select'&&!this.value&&v[0])this.value=v[0].value;}
      replaceChildren(){this.children=[];if(this.tag==='select')this.value='';}
      getContext(){return new Proxy({}, {get:()=>()=>{}});}
      getBoundingClientRect(){return {left:0,top:0,width:1280,height:720};}
      click(){return this.onclick?.();}
    }
    const elements={};
    for(const id of [...html.matchAll(/id="([^"]+)"/g)].map(m=>m[1]))elements[id]=new Element(['selected','type','mode'].includes(id)?'select':'div');
    elements.port.value='8081';elements.type.value='table';elements.mode.value='polygon';
    class ImageStub {constructor(){this.naturalWidth=1280;this.naturalHeight=720;}set src(v){queueMicrotask(()=>this.onload?.());}}
    const sandbox={document:{getElementById:id=>elements[id],createElement:tag=>new Element(tag),createTextNode:s=>s},
      location:{protocol:'http:',hostname:'127.0.0.1'},Image:ImageStub,structuredClone,console,confirm:()=>true,
      fetch:(url,opts)=>url.startsWith('/')?fetch(base+url,opts):Promise.resolve({ok:true,json:async()=>({enabled:true,revision:2,surfaces:[]})}),setInterval:()=>0};
    vm.createContext(sandbox);vm.runInContext(js,sandbox);await delay(150);
    // A blank ID must start a usable drawing draft, not silently reject Add.
    elements.newid.value='';elements.type.value='floor';elements.add.click();
    assert.match(elements['drawing-help'].textContent,/floor-1/);
    elements.canvas.onclick({clientX:100,clientY:100});
    assert.match(elements['drawing-help'].textContent,/1\/16/);
    elements.remove.click();
    elements.newid.value='wall-2';elements.type.value='wall';elements.add.click();
    assert.match(elements['drawing-help'].textContent,/0\/16/,'new surface resets unfinished points');
    for(const [x,y] of [[.01,.2],[.08,.2],[.08,.8],[.01,.8]])elements.canvas.onclick({clientX:x*1280,clientY:y*720});
    elements.finish.click();
    assert.equal(JSON.parse(elements.json.value).surfaces.length,2,'UI creates polygon draft');
    elements.enabled.checked=true;await elements.save.click();
    const saved=await (await fetch(base+'/api/surfaces')).json();
    assert.equal(saved.revision,3);assert.equal(saved.surfaces[1].type,'wall');
    // Normal baseline needs no per-object fixture registration.
    let captures=0,activeRevision=2;
    sandbox.fetch=(url,opts)=>url.startsWith('/')?fetch(base+url,opts):Promise.resolve({ok:true,json:async()=>{
      if(url.includes('/surface/capture')){captures++;assert.match(url,/id=wall-2&empty=0/);return {};}
      return {enabled:true,revision:activeRevision,surfaces:[]};
    }});
    await elements.capture.click();assert.equal(captures,0,'wait for detector revision');
    activeRevision=3;
    await elements.capture.click();assert.equal(captures,1,'baseline without fixtures');
    assert.match(elements.message.textContent,/5회/,'queued capture not reported complete');
    // Quick surface buttons retain click-based ROI creation.
    const drawPolygon=()=>{for(const [x,y] of [[400,200],[700,200],[700,400],[400,400]])elements.canvas.onclick({clientX:x,clientY:y});elements.finish.click();};
    elements['quick-table'].click();drawPolygon();
    let draft=JSON.parse(elements.json.value),quick=draft.surfaces.at(-1);
    assert.equal(quick.id,'table-2');assert.equal(quick.polygon.length,4);
    await elements.capture.click();assert.equal(captures,1,'unsaved changes block capture');
    await elements.save.click();
    assert.match(elements['drawing-help'].textContent,/사람이 앉는 곳/);
    drawPolygon();
    draft=JSON.parse(elements.json.value);assert.equal(draft.surfaces.at(-1).usage_zones.length,1);
    elements.cancel.click();
    console.log('Surface HTTP + UI workflow: PASS');
    console.log('Isolated fixture:',fixture);
  } finally { child.kill(); }
}
main().catch(e=>{console.error(e);process.exitCode=1;});
