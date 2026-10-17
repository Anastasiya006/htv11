const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const {WebSocketServer}=require('ws');
const pty=require('node-pty');
const screenshots=require('./screenshots.cjs');
const {randomUUID}=require('node:crypto');
// npm may unpack the macOS prebuilt helper without its executable bit.
if(process.platform==='darwin'){
 const helper=path.join(__dirname,'node_modules/node-pty/prebuilds',`darwin-${process.arch}`,'spawn-helper');
 if(fs.existsSync(helper))fs.chmodSync(helper,0o755);
}
const root=path.resolve(__dirname,'..');
const runtime=path.join(__dirname,'.runtime');
function state(){
 const now=Date.now();
 const sessions=[];
 for(const name of fs.existsSync(runtime)?fs.readdirSync(runtime):[]){
  if(!name.endsWith('.json')||name.endsWith('.response.json'))continue;
  try{const s=JSON.parse(fs.readFileSync(path.join(runtime,name),'utf8'));if(now-s.at<3600000&&s.event!=='SessionEnd'&&(!managedCLI||s.session===terminalSession))sessions.push(s);}catch{}
 }
 const priority={'needs-attention':4,working:3,reading:3,thinking:3,success:2,idle:1};
 sessions.sort((a,b)=>(priority[b.animation]||0)-(priority[a.animation]||0)||b.at-a.at);
 const active=sessions[0];
 const connected=managedCLI?Boolean(terminal):sessions.length>0;
 let response=null;
 for(const session of active?[active]:[]){
  try{const candidate=JSON.parse(fs.readFileSync(path.join(runtime,session.session+'.response.json'),'utf8'));if(candidate.at>=(session.turnAt||0)&&(!response||candidate.at>response.at))response=candidate;}catch{}
 }
 return {screenshots:connected?screenshots.list(active?.turnAt||Date.now()):[],animation:!connected?'disconnected':active?(active.animation==='success'&&now-active.at>5000?'idle':active.animation):'idle',connected,turnAt:active?.turnAt||0, message:active?.message||'', eventId:active?`${active.session}:${active.at}`:null, at:active?.at||0,response:response?.text||'',responseAt:response?.at||0};
}
const types={'.html':'text/html','.css':'text/css','.js':'text/javascript','.gif':'image/gif'};
const server=http.createServer((req,res)=>{
 res.setHeader('Cache-Control','no-store');
 const url=new URL(req.url,'http://127.0.0.1');
 if(req.method!=='GET'){res.writeHead(405);res.end();return;}
 if(url.pathname.startsWith('/screenshot/')){
  let name;try{name=decodeURIComponent(url.pathname.slice('/screenshot/'.length));}catch{res.writeHead(400);res.end();return;}
  const image=screenshots.image(name);
  if(!image){res.writeHead(404);res.end();return;}
  res.setHeader('Content-Type',image.type);res.setHeader('X-Content-Type-Options','nosniff');
  fs.createReadStream(image.file).on('error',()=>res.end()).pipe(res);return;
 }
 if(url.pathname==='/state'){res.setHeader('Content-Type','application/json');res.end(JSON.stringify(state()));return;}
 const vendors={'/vendor/xterm.js':'@xterm/xterm/lib/xterm.js','/vendor/xterm.css':'@xterm/xterm/css/xterm.css','/vendor/fit.js':'@xterm/addon-fit/lib/addon-fit.js'};
 if(vendors[url.pathname]){const f=path.join(__dirname,'node_modules',vendors[url.pathname]);res.setHeader('Content-Type',url.pathname.endsWith('.css')?'text/css':'text/javascript');fs.createReadStream(f).on('error',()=>res.end()).pipe(res);return;}
 const file=url.pathname==='/'?'simulator/index.html':url.pathname.slice(1);
 if(!['simulator/index.html','simulator/style.css','simulator/app.js','simulator/terminal.js','simulator/picker.js'].includes(file)&&!/^animations\/[a-z-]+\.gif$/.test(file)){res.writeHead(404);res.end();return;}
 fs.readFile(path.join(root,file),(err,data)=>{if(err){res.writeHead(404);res.end();return;}res.setHeader('Content-Type',types[path.extname(file)]||'application/octet-stream');res.end(data);});
});
server.on('error',err=>{console.error(err.message);process.exitCode=1;});
server.listen(3210,'127.0.0.1',()=>console.log('Gemmi: http://127.0.0.1:3210'));

const sockets=new WebSocketServer({noServer:true,maxPayload:65536});
let terminal=null, output='',client=null,managedCLI=false,terminalSession=null;
server.on('upgrade',(req,socket,head)=>{
 // A remote page must not be able to drive a local command terminal.
 if(req.url!=='/terminal'||req.headers.host!=='127.0.0.1:3210'||req.headers.origin!=='http://127.0.0.1:3210'){socket.destroy();return;}
 sockets.handleUpgrade(req,socket,head,ws=>sockets.emit('connection',ws));
});
sockets.on('connection',ws=>{
 if(client){ws.close(1008,'Terminal already open in another tab');return;}
 client=ws;
 const send=data=>{if(client?.readyState===1)client.send(JSON.stringify(data));};
 if(!terminal){
  output='';managedCLI=true;terminalSession=randomUUID();
  try{
   terminal=pty.spawn('gemini',['--session-id',terminalSession],{name:'xterm-256color',cols:80,rows:28,cwd:root,env:{...process.env,TERM:'xterm-256color'}});
   terminal.onData(data=>{output=(output+data).slice(-262144);send({type:'output',data});});
   terminal.onExit(({exitCode})=>{terminal=null;send({type:'exit',exitCode});});
  }catch(err){send({type:'output',data:'Cannot start Gemini CLI: '+err.message+'\r\n'});ws.close();}
 }else send({type:'output',data:output});
 ws.on('message',raw=>{
  try{const message=JSON.parse(raw);if(!terminal)return;
   if(message.type==='input'&&typeof message.data==='string')terminal.write(message.data);
   if(message.type==='resize'&&Number.isInteger(message.cols)&&Number.isInteger(message.rows))terminal.resize(Math.max(20,Math.min(300,message.cols)),Math.max(5,Math.min(100,message.rows)));
  }catch{}
 });
 ws.on('close',()=>{if(client===ws)client=null;});
});
function shutdown(){if(terminal)terminal.kill();server.close();process.exit();}
process.on('SIGTERM',shutdown);process.on('SIGINT',shutdown);
