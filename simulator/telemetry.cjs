// Versioned, bounded hook journal. Raw prompts, arguments and tool output stay out.
const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');
const validSession=id=>typeof id==='string'&&/^[a-zA-Z0-9_-]{1,128}$/.test(id);
function summarize(event){
 const input=event.tool_input||{};
 const tool=typeof event.tool_name==='string'?event.tool_name.slice(0,100):null;
 let target='';
 if(typeof input.file_path==='string')target=path.basename(input.file_path);
 else if(typeof input.path==='string')target=path.basename(input.path);
 else if(typeof input.command==='string')target=input.command.trim().split(/\s+/)[0].slice(0,40);
 if(target.includes('=')||/token|secret|password|api.?key/i.test(target))target='[redacted]';
 const error=Boolean(event.tool_response?.error);
 return {version:1,id:crypto.randomUUID(),at:Date.now(),event:event.hook_event_name,
  tool,target:target.slice(0,100),failed:error,
  call:tool?crypto.createHash('sha256').update(tool+JSON.stringify(input)).digest('hex').slice(0,16):null};
}
function record(runtime,event){
 if(!validSession(event.session_id))return;
 const directory=path.join(runtime,'events',event.session_id);
 fs.mkdirSync(directory,{recursive:true,mode:0o700});
 const entry=summarize(event);
 const name=`${entry.at}-${entry.id}.json`;
 fs.writeFileSync(path.join(directory,name),JSON.stringify(entry),{mode:0o600});
 const files=fs.readdirSync(directory).filter(n=>n.endsWith('.json')).sort();
 for(const old of files.slice(0,Math.max(0,files.length-512)))try{fs.unlinkSync(path.join(directory,old));}catch{}
}
function read(runtime,session,since=0,now=Date.now()){
 const entries=[];
 if(validSession(session)){
  const directory=path.join(runtime,'events',session);
  try{for(const name of fs.readdirSync(directory).filter(n=>n.endsWith('.json')).sort().slice(-512)){
   try{const entry=JSON.parse(fs.readFileSync(path.join(directory,name),'utf8'));if(entry.at>=since)entries.push(entry);}catch{}
  }}catch{}
 }
 entries.sort((a,b)=>a.at-b.at||a.id.localeCompare(b.id));
 const pending=new Map();const timeline=[];let tools=0,errors=0;
 for(const entry of entries){
  if(entry.event==='BeforeTool'){tools++;const queue=pending.get(entry.call)||[];queue.push(entry);pending.set(entry.call,queue);}
  let duration=null;
  if(entry.event==='AfterTool'){
   const start=pending.get(entry.call)?.shift();if(start)duration=Math.max(0,entry.at-start.at);
   if(entry.failed)errors++;
  }
  timeline.push({...entry,duration});
 }
 const started=entries.find(e=>e.event==='BeforeAgent');
 const ended=[...entries].reverse().find(e=>e.event==='AfterAgent');
 const running=[...pending.values()].flat().sort((a,b)=>b.at-a.at)[0]||null;
 return {version:1,tools,errors,duration:started?Math.max(0,(ended?.at||now)-started.at):0,
  running:running?{...running,duration:now-running.at}:null,timeline:timeline.slice(-128),lastEventAt:entries.at(-1)?.at||0};
}
module.exports={record,read,summarize};
