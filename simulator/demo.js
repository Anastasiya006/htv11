'use strict';
(() => {
 const button=document.getElementById('replay-demo');
 let timer=null,step=0,started=0,events=[],active=false;
 const steps=[
  ['BeforeAgent','thinking','Mapping out a plan…'],
  ['BeforeTool','reading','Reading the display driver…','read_file','lcd_display.c'],
  ['AfterTool','thinking','Display dimensions confirmed.','read_file','lcd_display.c'],
  ['BeforeTool','working','Running the checks…','run_shell_command','npm'],
  ['AfterTool','error','A check failed. Investigating…','run_shell_command','npm',true],
  ['BeforeTool','working','Updating the layout…','replace','style.css'],
  ['AfterTool','thinking','Layout updated.','replace','style.css'],
  ['BeforeTool','working','Checking the fix…','run_shell_command','npm'],
  ['AfterTool','success','Checks passed.','run_shell_command','npm'],
  ['AfterAgent','success','Preview ready.']
 ];
 function stop(){active=false;clearTimeout(timer);button.textContent='◇ Replay demo';document.getElementById('demo-banner').hidden=true;document.getElementById('connect').disabled=false;document.getElementById('terminal').classList.remove('demo-muted');window.dispatchEvent(new CustomEvent('gemmi-demo',{detail:false}));}
 function tick(){
  const [event,animation,message,tool,target,failed]=steps[step];
  const at=Date.now(),entry={id:'demo-'+step,event,at,tool,target,failed:!!failed,duration:event==='AfterTool'?1600:null};
  events.push(entry);
  const complete=step===steps.length-1;
  const snapshot={protocol:2,session:'demo-local-replay',connected:true,animation,message,turnAt:started,eventId:entry.id,at,latency:2,
   telemetry:{tools:events.filter(e=>e.event==='BeforeTool').length,errors:events.filter(e=>e.failed).length,duration:at-started,lastEventAt:at,timeline:[...events],running:event==='BeforeTool'?{tool,target,duration:0}:null},
   screenshots:complete?[{name:'Demo build preview',url:'/simulator/demo-preview.svg',at}]:[],
   response:complete?'Layout updated. The follow-up check passed.\n\nThis is a demonstration of the event pipeline: reading → command failure → edit → verification → screenshot.\n\nOpen the image card to inspect the sample preview. In live mode every event comes from Gemini CLI hooks.':'',responseAt:complete?at:0};
  window.dispatchEvent(new CustomEvent('gemmi-demo-state',{detail:snapshot}));
  step++;
  if(step<steps.length)timer=setTimeout(tick,1600);
  else button.textContent='■ Exit demo';
 }
 button.onclick=()=>{
  if(active){stop();return;}
  active=true;step=0;events=[];started=Date.now();
  window.dispatchEvent(new CustomEvent('gemmi-demo',{detail:true}));
  document.getElementById('demo-banner').hidden=false;document.getElementById('connect').disabled=true;
  document.getElementById('terminal').classList.add('demo-muted');
  button.textContent='■ Exit demo';tick();
 };
})();
