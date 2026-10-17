'use strict';
(() => {
 const device=document.getElementById('screen');
 const panel=document.createElement('section');panel.id='device-inspector';panel.hidden=true;
 panel.innerHTML='<div class="reader-pet"><img src="../animations/reading.gif" alt="Gemmi inspecting activity"></div><div class="reader-console"><span class="console-label">GEMMI / INSPECT</span><nav class="inspect-tabs" aria-label="Device inspector"><button data-panel="tools" class="selected">TOOLS</button><button data-panel="timeline">EVENTS</button><button data-panel="health">LINK</button></nav><div id="inspect-body" tabindex="0"></div><button id="inspect-back">CLOSE ↑</button></div>';
 device.append(panel);
 const body=document.getElementById('inspect-body');
 let selected='tools',snapshot={connected:false};
 const escaped=value=>String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
 const duration=ms=>ms<1000?`${Math.round(ms||0)}ms`:`${((ms||0)/1000).toFixed(1)}s`;
 const labels={SessionStart:'Session opened',BeforeAgent:'Thinking',BeforeTool:'Tool started',AfterTool:'Tool returned',AfterAgent:'Response ready',Notification:'Input requested',SessionEnd:'Session closed'};
 function open(name='tools'){
  selected=name;panel.hidden=false;
  document.querySelectorAll('[data-panel]').forEach(button=>{button.classList.toggle('selected',button.dataset.panel===name);button.setAttribute('aria-pressed',String(button.dataset.panel===name));});
  render();
 }
 document.querySelectorAll('[data-panel]').forEach(button=>button.onclick=()=>open(button.dataset.panel));
 function close(){panel.hidden=true;document.getElementById('device-status').focus();}
 document.getElementById('inspect-back').onclick=close;
 panel.addEventListener('keydown',event=>{if(event.key==='Escape')close();});
 window.addEventListener('gemmi-inspect',()=>open('tools'));
 window.addEventListener('gemmi-demo',()=>{panel.hidden=true;});
 function render(){
  if(panel.hidden)return;
  const data=snapshot.telemetry||{},entries=data.timeline||[],scroll=body.scrollTop;
  if(selected==='tools'){
   const calls=entries.filter(e=>e.event==='AfterTool');
   body.innerHTML=`<div class="device-metrics"><span><b>${data.tools||0}</b> calls</span><span><b>${data.errors||0}</b> errors</span><span>${duration(data.duration||0)}</span></div>`+
    (data.running?`<div class="device-running">▶ ${escaped(data.running.tool)}<small>${escaped(data.running.target||'')} · ${duration(data.running.duration)}</small></div>`:'')+
    (calls.length?[...calls].reverse().map(e=>`<article class="device-event"><span class="${e.failed?'bad':'good'}">${e.failed?'×':'✓'}</span><div>${escaped(e.tool)}<small>${escaped(e.target||'')}${e.target?' · ':''}${e.duration===null?'unpaired':duration(e.duration)}</small></div></article>`).join(''):'<p class="inspect-empty">No tool calls yet.<br>Send a prompt or replay the demo.</p>');
  }else if(selected==='timeline'){
   body.innerHTML=entries.length?[...entries].reverse().map(e=>`<article class="device-event"><time>${new Date(e.at).toLocaleTimeString([],{hour12:false,minute:'2-digit',second:'2-digit'})}</time><div>${escaped(labels[e.event]||e.event)}<small>${escaped(e.tool||'Gemini')}${e.failed?' · ERROR':''}</small></div></article>`).join(''):'<p class="inspect-empty">No events yet.<br>Waiting for a CLI turn.</p>';
  }else{
   body.innerHTML=`<dl class="device-health"><dt>CLI</dt><dd>${snapshot.demo?'DEMO REPLAY':snapshot.connected?'CONNECTED':'OFFLINE'}</dd><dt>ROUND TRIP</dt><dd>${snapshot.latency!==undefined?snapshot.latency+'ms':'—'}</dd><dt>LAST HOOK</dt><dd>${data.lastEventAt?duration(Math.max(0,Date.now()-data.lastEventAt))+' ago':'—'}</dd><dt>SESSION</dt><dd>${escaped(snapshot.session?.slice(0,8)||'—')}</dd><dt>PROTOCOL</dt><dd>v${snapshot.protocol||'—'} / loopback</dd></dl><button id="export-feed" ${entries.length?'':'disabled'}>EXPORT EVENTS ↓</button>`;
   document.getElementById('export-feed').onclick=()=>{
    const url=URL.createObjectURL(new Blob([JSON.stringify({version:1,demo:!!snapshot.demo,session:snapshot.session,events:entries},null,2)],{type:'application/json'}));
    const link=document.createElement('a');link.href=url;link.download='gemmi-events.json';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
   };
  }
  body.scrollTop=scroll;
 }
 window.addEventListener('gemmi-state',event=>{snapshot=event.detail;render();});
})();
