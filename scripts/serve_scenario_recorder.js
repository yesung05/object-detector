/* Loopback-only, read-only static server. Videos are written by browser folder permission. */
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const {scenariosFromMarkdown}=require('./recording_scenarios');
const {buildRetakePlan}=require('./recording_retake_plan');
const root=path.resolve(__dirname,'..');
function createServer(){
 const scenarios=scenariosFromMarkdown(fs.readFileSync(path.join(root,'docs/논문/50-scenarios-recording-plan.md'),'utf8'));
 const routes={'/':['dashboard/scenario-recorder.html','text/html'], '/recorder.js':['dashboard/scenario-recorder.js','text/javascript'], '/phases.js':['scripts/recording_scenarios.js','text/javascript'], '/browser-voice.js':['dashboard/browser-voice.js','text/javascript']};
 return http.createServer((req,res)=>{
  if(req.method!=='GET'){res.writeHead(405);res.end();return;}
  const url=new URL(req.url,'http://localhost');
  res.setHeader('Cache-Control','no-store');res.setHeader('X-Content-Type-Options','nosniff');
  if(url.pathname==='/api/scenarios'){res.setHeader('Content-Type','application/json; charset=utf-8');res.end(JSON.stringify(scenarios));return;}
  if(url.pathname==='/api/retake-plan'){try{res.setHeader('Content-Type','application/json; charset=utf-8');res.end(JSON.stringify(buildRetakePlan(path.join(root,'records'),scenarios)));}catch(e){res.writeHead(500);res.end(JSON.stringify({error:'촬영 기록을 읽지 못했습니다.'}));}return;}
  const route=routes[url.pathname];if(!route){res.writeHead(404);res.end('Not found');return;}
  res.setHeader('Content-Type',route[1]+'; charset=utf-8');fs.createReadStream(path.join(root,route[0])).pipe(res);
 });
}
if(require.main===module){const server=createServer();server.on('error',e=>{console.error('Recorder server:',e.message);process.exitCode=1;});server.listen(8092,'127.0.0.1',()=>{console.log('Open http://127.0.0.1:8092 in Chrome or Edge. Keep this window open. Ctrl+C to stop.');if(process.argv.includes('--open')&&process.platform==='win32')require('node:child_process').execFile('explorer.exe',['http://127.0.0.1:8092'],()=>{});});}
module.exports={createServer};
