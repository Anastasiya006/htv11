'use strict';
const terminal=new Terminal({cursorBlink:true,fontSize:13,fontFamily:'Menlo, monospace',theme:{background:'#000000',foreground:'#d4cddd'},scrollback:3000});
const fit=new FitAddon.FitAddon();terminal.loadAddon(fit);terminal.open(document.getElementById('terminal'));fit.fit();
terminal.writeln('Start CLI to open Gemini here.');
const connect=document.getElementById('connect');let socket;
function send(message){if(!document.getElementById('demo-banner').hidden)return;if(socket?.readyState===WebSocket.OPEN)socket.send(JSON.stringify(message));}
terminal.onData(data=>send({type:'input',data}));
const resize=()=>{if(document.getElementById('terminal').hidden)return;fit.fit();send({type:'resize',cols:terminal.cols,rows:terminal.rows});};
new ResizeObserver(resize).observe(document.getElementById('terminal'));
window.addEventListener('resize',()=>requestAnimationFrame(resize));
document.fonts.ready.then(resize);
connect.onclick=()=>{
 if(document.getElementById('demo-banner')&&!document.getElementById('demo-banner').hidden)return;
 if(socket&&socket.readyState<2)return;
 terminal.reset();connect.disabled=true;connect.textContent='Connecting…';
 socket=new WebSocket(`ws://${location.host}/terminal`);
 socket.onopen=()=>{window.dispatchEvent(new CustomEvent('gemmi-connection',{detail:{connected:true}}));connect.textContent='Connected';resize();terminal.focus();};
 socket.onmessage=event=>{const message=JSON.parse(event.data);if(message.type==='output'){terminal.write(message.data);}if(message.type==='exit'){terminal.writeln(`\r\nGemini exited (${message.exitCode}).`);socket.close();}};
 socket.onclose=event=>{window.dispatchEvent(new CustomEvent('gemmi-connection',{detail:{connected:false}}));connect.disabled=false;connect.textContent='Reconnect CLI';terminal.writeln('\r\nDisconnected.'+(event.reason?' '+event.reason:''));};
 socket.onerror=()=>terminal.writeln('\r\nCannot reach the local CLI bridge.');
};

const picker=document.createElement('section');picker.id='input-picker';picker.hidden=true;
picker.innerHTML='<div class="picker-pet"><img src="../animations/needs-attention.gif" alt="Gemmi needs your input"></div><div class="picker-console"><span class="console-label">GEMMI</span><div class="picker-scroll"><p id="picker-context"></p><div id="picker-options"></div></div><div class="picker-footer"><button id="picker-later">BACK</button><button id="picker-confirm" disabled>CONFIRM ↵</button></div></div>';
document.getElementById('screen').append(picker);
let menu=null,chosen=null,dismissed=null;
function currentMenu(){
 const buffer=terminal.buffer.active,lines=[];
 for(let i=buffer.baseY;i<buffer.baseY+terminal.rows;i++)lines.push(buffer.getLine(i)?.translateToString(true)||'');
 return readGemmiMenu(lines);
}
function drawMenu(){
 const next=socket?.readyState===WebSocket.OPEN&&document.getElementById('demo-banner').hidden?currentMenu():null;
 if(!next){menu=null;chosen=null;picker.hidden=true;dismissed=null;return;}
 if(menu?.signature===next.signature)return;
 menu=next;chosen=null;
 if(dismissed===menu.signature)return;
 picker.hidden=false;
 document.getElementById('picker-context').textContent=menu.context||'Choose an option';
 const options=document.getElementById('picker-options');options.replaceChildren();
 document.getElementById('picker-confirm').disabled=true;
 menu.options.forEach((text,index)=>{
  const button=document.createElement('button');button.textContent='○ '+text;
  button.onclick=()=>{chosen=index;Array.from(options.children).forEach((b,i)=>{b.textContent=(i===index?'● ':'○ ')+menu.options[i];});document.getElementById('picker-confirm').disabled=false;};
  options.append(button);
 });
}
terminal.onWriteParsed(drawMenu);
document.getElementById('picker-later').onclick=()=>{dismissed=menu?.signature;picker.hidden=true;terminal.focus();};
document.getElementById('picker-confirm').onclick=()=>{
 const latest=currentMenu();
 if(!menu||chosen===null||latest?.signature!==menu.signature){menu=null;drawMenu();return;}
 const delta=chosen-latest.selected;
 send({type:'input',data:(delta>0?'\x1b[B':'\x1b[A').repeat(Math.abs(delta))+'\r'});
 dismissed=menu.signature;picker.hidden=true;chosen=null;
};
window.addEventListener('gemmi-connection',event=>{if(!event.detail.connected){picker.hidden=true;menu=null;chosen=null;}});

window.addEventListener('gemmi-demo',event=>{picker.hidden=true;menu=null;chosen=null;if(!event.detail)setTimeout(drawMenu,0);});

// Mirror only live spinner/status lines, not input, menus or terminal chrome.
let lastProgress=null;
terminal.onWriteParsed(()=>{
 let progress=null;
 if(socket?.readyState===WebSocket.OPEN&&document.getElementById('demo-banner').hidden){
  const buffer=terminal.buffer.active;
  for(let row=buffer.baseY+terminal.rows-1;row>=buffer.baseY;row--){
   const line=buffer.getLine(row)?.translateToString(true)||'';
   const match=line.match(/^\s*[│┃]?\s*([\u2801-\u28ff◐◓◑◒✢✳✶✻✽])\s+(.+?)\s*[│┃]?\s*$/u);
   if(match){progress=match[1]+' '+match[2];break;}
  }
 }
 if(progress!==lastProgress){lastProgress=progress;window.dispatchEvent(new CustomEvent('gemmi-progress',{detail:progress}));}
});
window.addEventListener('gemmi-connection',event=>{if(!event.detail.connected){lastProgress=null;window.dispatchEvent(new CustomEvent('gemmi-progress',{detail:null}));}});
