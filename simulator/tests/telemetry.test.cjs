const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {record,read,summarize}=require('../telemetry.cjs');
test('journal pairs tools, measures duration, and retains errors',()=>{
 const runtime=fs.mkdtempSync(path.join(os.tmpdir(),'gemmi-events-'));
 try{
  const directory=path.join(runtime,'events','unit-session');fs.mkdirSync(directory,{recursive:true});
  const events=[{id:'1',at:100,event:'BeforeAgent'},{id:'2',at:120,event:'BeforeTool',tool:'read_file',call:'read'},{id:'3',at:220,event:'AfterTool',tool:'read_file',call:'read',failed:false},{id:'4',at:250,event:'BeforeTool',tool:'run_shell_command',call:'shell'},{id:'5',at:450,event:'AfterTool',tool:'run_shell_command',call:'shell',failed:true},{id:'6',at:500,event:'AfterAgent'}];
  events.forEach(e=>fs.writeFileSync(path.join(directory,e.id+'.json'),JSON.stringify(e)));
  const result=read(runtime,'unit-session',100,600);
  assert.equal(result.tools,2);assert.equal(result.errors,1);assert.equal(result.duration,400);
  assert.equal(result.timeline[2].duration,100);assert.equal(result.timeline[4].duration,200);assert.equal(result.running,null);
  assert.equal(read(runtime,'unit-session',300).tools,0);
  assert.equal(read(runtime,'../unit-session').timeline.length,0);
 }finally{fs.rmSync(runtime,{recursive:true,force:true});}
});
test('telemetry excludes prompt, arguments, output and environment assignments',()=>{
 const entry=summarize({hook_event_name:'BeforeTool',prompt:'private prompt',tool_name:'run_shell_command',tool_input:{command:'API_KEY=private-value npm test'},tool_response:{llmContent:'private output'}});
 const text=JSON.stringify(entry);
 assert(!text.includes('private'));assert.equal(entry.target,'[redacted]');assert.equal(entry.call.length,16);
});
test('journal has a bounded retention and running tool state',()=>{
 const runtime=fs.mkdtempSync(path.join(os.tmpdir(),'gemmi-bound-'));
 try{
  const directory=path.join(runtime,'events','unit-session');fs.mkdirSync(directory,{recursive:true});
  for(let i=0;i<515;i++)fs.writeFileSync(path.join(directory,`${String(i).padStart(4,'0')}.json`),JSON.stringify({id:String(i),at:i,event:'Notification'}));
  record(runtime,{session_id:'unit-session',hook_event_name:'BeforeTool',tool_name:'read_file',tool_input:{file_path:'/private/project/main.c'}});
  assert.equal(fs.readdirSync(directory).length,512);
  const result=read(runtime,'unit-session');assert.equal(result.timeline.length,128);assert.equal(result.running.target,'main.c');
 }finally{fs.rmSync(runtime,{recursive:true,force:true});}
});
