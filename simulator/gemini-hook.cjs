// Observability only. Stores completed assistant responses for the device display.
const fs = require('node:fs');
const path = require('node:path');
const dir = path.join(__dirname, '.runtime');
let input = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', chunk => { input += chunk; });
process.stdin.on('end', () => {
 try {
  const event = JSON.parse(input);
  if (!event.session_id || !/^[a-zA-Z0-9_-]{1,128}$/.test(event.session_id)) return;
  const states = {SessionStart:'idle', BeforeAgent:'thinking', BeforeTool:'working', AfterTool:'thinking', AfterAgent:'success', Notification:'needs-attention', SessionEnd:'idle'};
  const readingTools=new Set(['read_file','read_many_files','list_directory','glob','grep_search','search_file_content','google_web_search','web_fetch']);
  const animation = event.hook_event_name==='BeforeTool'&&readingTools.has(event.tool_name)?'reading':states[event.hook_event_name];
  if (!animation) return;
  const toolMessages = {read_file:'Reading a file…', read_many_files:'Reading files…', list_directory:'Looking around…', glob:'Finding files…', grep_search:'Searching the code…', search_file_content:'Searching the code…', write_file:'Writing a file…', replace:'Making a change…', run_shell_command:'Running a command…', google_web_search:'Searching the web…', web_fetch:'Reading a page…', ask_user:'A little help?'};
  const messages = {SessionStart:'Hi! Ready when you are.', BeforeAgent:'Let me think…', BeforeTool:toolMessages[event.tool_name] || 'Working on it…', AfterTool:'What’s next…', AfterAgent:'All done!', Notification:'A little help? Check the CLI.', SessionEnd:'See you soon.'};
  const message = messages[event.hook_event_name];
  fs.mkdirSync(dir, {recursive:true, mode:0o700});
  const file = path.join(dir, event.session_id + '.json');
  let turnAt=0;
  try{turnAt=JSON.parse(fs.readFileSync(file,'utf8')).turnAt||0;}catch{}
  if(event.hook_event_name==='BeforeAgent'){turnAt=Date.now();try{fs.unlinkSync(path.join(dir,event.session_id+'.response.json'));}catch{}}
  const temp = file + '.' + process.pid + '.tmp';
  fs.writeFileSync(temp, JSON.stringify({session:event.session_id, animation, message, turnAt, event:event.hook_event_name, at:Date.now()}), {mode:0o600});
  fs.renameSync(temp, file);
  if(event.hook_event_name==='AfterAgent'&&typeof event.prompt_response==='string'){
   const responseFile=path.join(dir,event.session_id+'.response.json');
   const responseTemp=responseFile+'.'+process.pid+'.tmp';
   fs.writeFileSync(responseTemp,JSON.stringify({text:event.prompt_response.trim().slice(-32768),at:Date.now()}),{mode:0o600});
   fs.renameSync(responseTemp,responseFile);
  }
 } catch { /* A missing display must never interrupt the CLI. */ }
 finally { process.stdout.write('{}\n'); }
});
