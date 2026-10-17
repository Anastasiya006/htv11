'use strict';
const screen=document.getElementById('screen');
screen.innerHTML='<button id="device-status" title="Open session inspector"><span id="device-signal">○ offline</span><span id="device-clock"></span></button><button class="pet" aria-label="Pet Gemmi"><img src="../animations/idle.gif" alt="Gemmi"></button><div class="cli-status"><span class="console-label">gemmi</span><div id="activity" role="status">Waiting for Gemini…</div><button id="image-card" hidden>▧ view image <span id="image-count"></span> ↗</button><button id="response-preview" hidden><span id="response-text"></span><span class="read-more">more ↓</span></button></div><section id="response-view" hidden aria-label="Full Gemmi response"><div class="reader-pet"><img src="../animations/reading.gif" alt="Gemmi reading"></div><div class="reader-console"><span class="console-label">gemmi</span><button id="reader-image" hidden>▧ view image ↗</button><div id="response-body" tabindex="0"></div><button id="response-back">close ↑</button></div></section>';
const imageView=document.createElement('section');imageView.id='image-view';imageView.hidden=true;
imageView.innerHTML='<header><button id="image-back">‹ BACK</button><span id="image-page"></span><button id="image-zoom">ZOOM +</button></header><div id="image-stage"><img id="screenshot" alt="Agent screenshot"></div><footer><button id="image-prev">←</button><span id="image-name"></span><button id="image-next">→</button></footer><p id="image-error" hidden>Image unavailable.</p>';
screen.append(imageView);
const imageCard=document.getElementById('image-card');
const readerImage=document.getElementById('reader-image');
const screenshot=document.getElementById('screenshot');
let images=[],imageIndex=0,zoomed=false;
function showImage(){
 const image=images[imageIndex];if(!image)return;
 responseView.hidden=true;imageView.hidden=false;zoomed=false;screenshot.classList.remove('zoomed');
 screenshot.src=image.url;screenshot.alt=image.name;
 document.getElementById('image-name').textContent=image.name;
 document.getElementById('image-page').textContent=`${imageIndex+1}/${images.length}`;
 document.getElementById('image-zoom').textContent='ZOOM +';
 document.getElementById('image-error').hidden=true;
 document.getElementById('image-prev').disabled=imageIndex===0;
 document.getElementById('image-next').disabled=imageIndex===images.length-1;
}
imageCard.onclick=()=>{imageIndex=0;showImage();};
readerImage.onclick=imageCard.onclick;
document.getElementById('image-back').onclick=()=>{imageView.hidden=true;imageCard.focus();};
document.getElementById('image-prev').onclick=()=>{if(imageIndex>0){imageIndex--;showImage();}};
document.getElementById('image-next').onclick=()=>{if(imageIndex<images.length-1){imageIndex++;showImage();}};
document.getElementById('image-zoom').onclick=()=>{zoomed=!zoomed;screenshot.classList.toggle('zoomed',zoomed);document.getElementById('image-zoom').textContent=zoomed?'FIT':'ZOOM +';};
screenshot.onerror=()=>{document.getElementById('image-error').hidden=false;};
imageView.addEventListener('keydown',event=>{if(event.key==='Escape'){imageView.hidden=true;imageCard.focus();}});
const img=screen.querySelector('img');
const animations=new Set(['idle','thinking','reading','working','success','needs-attention','disconnected','error','blink','look-left','look-right','sleeping','waking-up']);
let current='idle',reactionUntil=0,lastEvent=null,lastResponse=null,lastTurn=null;
let terminalOffline=true,demoMode=false,latestState=null,liveProgress=null;
let idleSince=Date.now(),idleBeat=-1;
const activity=document.getElementById('activity');
const preview=document.getElementById('response-preview');
const responseText=document.getElementById('response-text');
const responseView=document.getElementById('response-view');
const responseBody=document.getElementById('response-body');
const moreLabel=preview.querySelector('.read-more');
function updatePreview(){
 const clipped=!preview.hidden&&responseText.scrollHeight>responseText.clientHeight+1;
 moreLabel.hidden=!clipped;
 preview.disabled=!clipped;
}
new ResizeObserver(updatePreview).observe(responseText);
let fullResponse='';
function clearResponse(){
 fullResponse='';lastResponse=null;images=[];imageCard.hidden=true;readerImage.hidden=true;imageView.hidden=true;
 preview.hidden=true;responseText.textContent='';responseBody.textContent='';responseView.hidden=true;
}
window.addEventListener('gemmi-progress',event=>{
 if(demoMode||terminalOffline)return;
 liveProgress=event.detail;
 activity.textContent=liveProgress||latestState?.message||'Ready.';
});
window.addEventListener('gemmi-connection',event=>{
 if(demoMode)return;
 terminalOffline=!event.detail.connected;
 if(terminalOffline){liveProgress=null;clearResponse();reactionUntil=0;latestState={connected:false,animation:'disconnected'};display('disconnected');document.getElementById('device-signal').textContent='○ offline';activity.textContent='CLI disconnected.';}
});
function print(text,response=false){
 if(!text)return;
 if(response){
  fullResponse=text.trim();
  responseText.textContent=text.replace(/\s+/g,' ').trim();
  preview.hidden=false;
  updatePreview();
  // Do not replace or scroll an answer the user is currently reading.
 }else activity.textContent=liveProgress||text;
}
preview.onclick=()=>{
 if(preview.disabled)return;
 responseBody.textContent=fullResponse;
 responseView.hidden=false;
 responseBody.scrollTop=0;
 document.getElementById('response-back').focus();
};
function closeResponse(){responseView.hidden=true;preview.focus();}
document.getElementById('response-back').onclick=closeResponse;
responseView.addEventListener('keydown',event=>{if(event.key==='Escape')closeResponse();});
function display(animation){if(current!==animation){current=animation;img.src=`../animations/${animation}.gif`;}}
screen.querySelector('.pet').onclick=()=>{
 reactionUntil=Date.now()+2200;idleSince=Date.now();
 display(current==='sleeping'?'waking-up':'love');
};
document.getElementById('device-status').onclick=()=>window.dispatchEvent(new CustomEvent('gemmi-inspect'));
setInterval(()=>{
 if(!latestState)return;
 const busy=['thinking','reading','working','needs-attention','error'].includes(latestState.animation);
 if(busy||!latestState.connected||Date.now()<reactionUntil)return;
 const elapsed=Date.now()-idleSince;
 const beat=Math.floor(elapsed/1000);
 if(beat===idleBeat)return;idleBeat=beat;
 if(elapsed>60000)display('sleeping');
 else if(beat%20===12)display('look-left');
 else if(beat%20===14)display('look-right');
 else if(beat%9===7)display('blink');
 else display(latestState.animation==='success'&&elapsed<5000?'success':'idle');
},500);
function applyState(state){
 if(!demoMode&&terminalOffline)state={...state,connected:false,animation:'disconnected',response:'',screenshots:[]};
 const changed=latestState?.eventId!==state.eventId;
 if(changed){idleSince=Date.now();idleBeat=-1;}
 latestState=state;
 document.getElementById('device-signal').textContent=(demoMode?'◇ demo':state.connected?'● '+(state.animation||'idle'):'○ offline');
 document.getElementById('device-clock').textContent=state.telemetry?.running?Math.floor(state.telemetry.running.duration/1000)+'s':'';
 window.dispatchEvent(new CustomEvent('gemmi-state',{detail:{...state,demo:demoMode}}));

  if(state.turnAt&&state.turnAt!==lastTurn){lastTurn=state.turnAt;clearResponse();}
  if(!state.connected||(!demoMode&&terminalOffline)){clearResponse();reactionUntil=0;display('disconnected');activity.textContent='CLI disconnected.';return;}
  const nextImages=Array.isArray(state.screenshots)?state.screenshots:[];
  // Keep an open image stable while further screenshots arrive.
  if(imageView.hidden)images=nextImages;
  imageCard.hidden=images.length===0;
  readerImage.hidden=images.length===0;
  document.getElementById('image-count').textContent=images.length>1?`(${images.length})`:'';
  const eventId=state.eventId||`${state.connected}:${state.animation}`;
  const newResponse=state.response&&state.responseAt!==lastResponse;
  if(eventId!==lastEvent){
   lastEvent=eventId;
   const labels={thinking:'Thinking…',reading:'Reading…',working:'Working…',success:'Done.', 'needs-attention':'Waiting for your input…',idle:state.connected?'Ready.':'Waiting for Gemini…'};
   if(!(newResponse&&state.animation==='success'))print(state.message||labels[state.animation]||'');
  }
  if(newResponse){
   lastResponse=state.responseAt;
   print(state.response,true);
  }
  screen.title=state.connected?'Gemini CLI connected':'Waiting for Gemini CLI';
  if(Date.now()>reactionUntil&&(changed||!['idle','success'].includes(state.animation)))display(animations.has(state.animation)?state.animation:'idle');
}
async function update(){
 try{
  if(demoMode)return;
  const started=performance.now();
  const response=await fetch('/state',{cache:'no-store',signal:AbortSignal.timeout(2000)});
  if(!response.ok)throw Error('Bridge unavailable');
  const state=await response.json();
  if(demoMode)return;
  state.latency=Math.round(performance.now()-started);
  applyState(state);
 }catch{
  if(demoMode)return;
  latestState={connected:false,animation:'disconnected'};
  document.getElementById('device-signal').textContent='○ offline';
  window.dispatchEvent(new CustomEvent('gemmi-state',{detail:latestState}));
  clearResponse();reactionUntil=0;
  if(lastEvent!=='offline'){lastEvent='offline';print('CLI bridge disconnected.');}
  screen.title='Start the local bridge: node simulator/server.cjs';
  if(Date.now()>reactionUntil)display('disconnected');
 }finally{setTimeout(update,500);}
}
window.addEventListener('gemmi-demo',event=>{
 demoMode=Boolean(event.detail);liveProgress=null;
 clearResponse();lastTurn=null;lastEvent=null;latestState=null;
 if(!demoMode){idleSince=Date.now();display('disconnected');}
});
window.addEventListener('gemmi-demo-state',event=>{if(demoMode)applyState(event.detail);});
update();

const zoomControl=document.getElementById('device-zoom');
function setZoom(){
 const zoom=Number(zoomControl.value);
 document.getElementById('device-viewport').style.width=240*zoom+'px';
 document.getElementById('device-viewport').style.height=320*zoom+'px';
 document.querySelector('.device-column').style.width=240*zoom+'px';
 screen.style.transform=`scale(${zoom})`;screen.style.transformOrigin='top left';
}
if(innerWidth<850)zoomControl.value='1';
zoomControl.onchange=setZoom;setZoom();
