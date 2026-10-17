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
 return {options,selected,context:lines.slice(Math.max(0,first-7),first).map(l=>l.replace(/[\u2500-\u257f]/g,'').trim()).filter(Boolean).join('\n'),signature:lines.slice(Math.max(0,first-7),last+1).join('\n')};
}
if(typeof module!=='undefined')module.exports={readGemmiMenu};
