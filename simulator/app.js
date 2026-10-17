'use strict';
const screen=document.getElementById('screen');
screen.innerHTML='<button class="pet" aria-label="Pet Gemmi"><img src="../animations/idle.gif" alt="Gemmi"></button><div class="cli-status"><span class="console-label">GEMMI</span><div id="activity" role="status">Waiting for Gemini…</div><button id="image-card" hidden>▧ View image <span id="image-count"></span> ↗</button><button id="response-preview" hidden><span id="response-text"></span><span class="read-more">MORE ↓</span></button></div><section id="response-view" hidden aria-label="Full Gemmi response"><div class="reader-pet"><img src="../animations/reading.gif" alt="Gemmi reading"></div><div class="reader-console"><span class="console-label">GEMMI</span><button id="reader-image" hidden>▧ VIEW IMAGE ↗</button><div id="response-body" tabindex="0"></div><button id="response-back">CLOSE ↑</button></div></section>';
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
const animations=new Set(['idle','thinking','reading','working','success','needs-attention','disconnected']);
let current='idle',reactionUntil=0,lastEvent=null,lastResponse=null,lastTurn=null;
let terminalOffline=false;
const activity=document.getElementById('activity');
const preview=document.getElementById('response-preview');
const responseText=document.getElementById('response-text');
const responseView=document.getElementById('response-view');
const responseBody=document.getElementById('response-body');
let fullResponse='';
function clearResponse(){
 fullResponse='';lastResponse=null;images=[];imageCard.hidden=true;readerImage.hidden=true;imageView.hidden=true;
 preview.hidden=true;responseText.textContent='';responseBody.textContent='';responseView.hidden=true;
}
window.addEventListener('gemmi-connection',event=>{
 terminalOffline=!event.detail.connected;
 if(terminalOffline){clearResponse();reactionUntil=0;display('disconnected');activity.textContent='CLI disconnected.';}
});
function print(text,response=false){
 if(!text)return;
 if(response){
  fullResponse=text.trim();
  responseText.textContent=text.replace(/\s+/g,' ').trim();
  preview.hidden=false;
  // Do not replace or scroll an answer the user is currently reading.
 }else activity.textContent=text;
}
preview.onclick=()=>{
 responseBody.textContent=fullResponse;
 responseView.hidden=false;
 responseBody.scrollTop=0;
 document.getElementById('response-back').focus();
};
function closeResponse(){responseView.hidden=true;preview.focus();}
document.getElementById('response-back').onclick=closeResponse;
responseView.addEventListener('keydown',event=>{if(event.key==='Escape')closeResponse();});
function display(animation){if(current!==animation){current=animation;img.src=`../animations/${animation}.gif`;}}
screen.querySelector('.pet').onclick=()=>{reactionUntil=Date.now()+2200;display('love');};
async function update(){
 try{
  const response=await fetch('/state',{cache:'no-store',signal:AbortSignal.timeout(2000)});
  if(!response.ok)throw Error('Bridge unavailable');
  const state=await response.json();
  if(state.turnAt&&state.turnAt!==lastTurn){lastTurn=state.turnAt;clearResponse();}
  if(!state.connected||terminalOffline){clearResponse();reactionUntil=0;display('disconnected');activity.textContent='CLI disconnected.';return;}
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
  if(Date.now()>reactionUntil)display(animations.has(state.animation)?state.animation:'idle');
 }catch{
  clearResponse();reactionUntil=0;
  if(lastEvent!=='offline'){lastEvent='offline';print('CLI bridge disconnected.');}
  screen.title='Start the local bridge: node simulator/server.cjs';
  if(Date.now()>reactionUntil)display('disconnected');
 }finally{setTimeout(update,500);}
}
update();
