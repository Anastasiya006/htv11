'use strict';
// Detect numbered radio menus in the parsed, visible terminal screen only.
function readGemmiMenu(lines){
 const options=[];let selected=-1,first=-1,last=-1;
 for(let i=0;i<lines.length;i++){
  const m=lines[i].match(/^\s*[│┃]?\s*([❯›>●◉○ ]?)\s*(\d+)[.)]\s+(.+?)(?:\s*[│┃])?\s*$/u);
  if(!m)continue;
  if(options.length&&Number(m[2])!==options.length+1){options.length=0;selected=-1;first=-1;}
  if(!options.length&&Number(m[2])!==1)continue;
  if(first<0)first=i;
  if(/[❯›>●◉]/u.test(m[1]))selected=options.length;
  options.push(m[3].trim());last=i;
 }
 if(options.length<2||selected<0)return null;
 return {options,selected,context:lines.slice(Math.max(0,first-7),first).map(l=>l.replace(/[\u2500-\u257f]/g,'').trim()).filter(Boolean).join('\n'),signature:lines.slice(first,last+1).join('\n')};
}

'use strict';
const terminal=new Terminal({cursorBlink:true,fontSize:13,fontFamily:'Menlo, monospace',theme:{background:'#000000',foreground:'#d4cddd'},scrollback:3000});
const fit=new FitAddon.FitAddon();terminal.loadAddon(fit);terminal.open(document.getElementById('terminal'));fit.fit();
terminal.writeln('Start CLI to open Gemini here.');
const connect=document.getElementById('connect');let socket;
function send(message){if(socket?.readyState===WebSocket.OPEN)socket.send(JSON.stringify(message));}
terminal.onData(data=>send({type:'input',data}));
const resize=()=>{fit.fit();send({type:'resize',cols:terminal.cols,rows:terminal.rows});};
new ResizeObserver(resize).observe(document.getElementById('terminal'));
connect.onclick=()=>{
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
 const next=socket?.readyState===WebSocket.OPEN?currentMenu():null;
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
