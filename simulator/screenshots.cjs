const fs=require('node:fs');
const path=require('node:path');
const directory=path.resolve(__dirname,'../screenshots');
function image(name){
 if(typeof name!=='string'||path.basename(name)!==name||! /\.(png|jpe?g|webp)$/i.test(name))return null;
 try{
  const file=path.join(directory,name);
  if(fs.realpathSync(file)!==file)return null;
  const stat=fs.statSync(file);
  if(!stat.isFile()||stat.size>10*1024*1024)return null;
  const fd=fs.openSync(file,'r'),header=Buffer.alloc(12);try{fs.readSync(fd,header,0,12,0);}finally{fs.closeSync(fd);}
  let type;
  if(header.subarray(0,8).equals(Buffer.from([137,80,78,71,13,10,26,10])))type='image/png';
  else if(header[0]===255&&header[1]===216&&header[2]===255)type='image/jpeg';
  else if(header.toString('ascii',0,4)==='RIFF'&&header.toString('ascii',8,12)==='WEBP')type='image/webp';
  if(!type)return null;
  return {file,type,name,at:stat.mtimeMs,size:stat.size,url:'/screenshot/'+encodeURIComponent(name)+'?v='+stat.mtimeMs};
 }catch{return null;}
}
function list(since=0){
 try{return fs.readdirSync(directory).map(image).filter(x=>x&&x.at>=since).sort((a,b)=>b.at-a.at).slice(0,8).map(({file,type,...publicImage})=>publicImage);}catch{return [];}
}
module.exports={image,list};
