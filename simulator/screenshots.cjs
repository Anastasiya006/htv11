const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');
const root=fs.realpathSync(path.resolve(__dirname,'..'));
const directory=path.join(root,'screenshots');
function image(name){
 if(typeof name!=='string'||! /\.(png|jpe?g|webp)$/i.test(name))return null;
 try{
  // Bare names retain the original root screenshots/ convention.
  const file=path.resolve(root,name.includes('/')?name:path.join('screenshots',name));
  const relative=path.relative(root,file);
  if(!relative||relative.startsWith('..')||path.isAbsolute(relative)||relative.split(path.sep).some(p=>p.startsWith('.')))return null;
  if(fs.realpathSync(file)!==file)return null;
  const stat=fs.statSync(file);
  if(!stat.isFile()||stat.size>10*1024*1024)return null;
  const fd=fs.openSync(file,'r'),header=Buffer.alloc(12);try{fs.readSync(fd,header,0,12,0);}finally{fs.closeSync(fd);}
  let type;
  if(header.subarray(0,8).equals(Buffer.from([137,80,78,71,13,10,26,10])))type='image/png';
  else if(header[0]===255&&header[1]===216&&header[2]===255)type='image/jpeg';
  else if(header.toString('ascii',0,4)==='RIFF'&&header.toString('ascii',8,12)==='WEBP')type='image/webp';
  if(!type)return null;
  return {file,type,name:path.basename(file),at:stat.mtimeMs,size:stat.size,url:'/screenshot/'+encodeURIComponent(relative)+'?v='+stat.mtimeMs};
 }catch{return null;}
}
function list(since=0,response=''){
 const candidates=[];
 try{for(const name of fs.readdirSync(directory)){const found=image(name);if(found&&found.at>=since)candidates.push(found);}}catch{}
 for(const match of response.matchAll(/!\[[^\]]*\]\((?:<([^>]+)>|([^\n)]+))\)/g)){
  try{
   const name=decodeURIComponent((match[1]||match[2]).trim());
   if(/^[a-z]+:\/\//i.test(name))continue;
   const found=image(name);
   if(found){
    const relative=path.relative(root,found.file);
    const imported='capture-'+crypto.createHash('sha256').update(relative).digest('hex').slice(0,8)+'-'+found.name;
    if(!candidates.some(item=>item.name===imported))candidates.push(found);
   }
  }catch{}
 }
 const unique=[...new Map(candidates.map(item=>[item.file,item])).values()];
 return unique.sort((a,b)=>b.at-a.at).slice(0,8).map(({file,type,...publicImage})=>publicImage);
}
function importReferences(response){
 fs.mkdirSync(directory,{recursive:true});
 const found=list(Number.MAX_SAFE_INTEGER,response);
 for(const attachment of found){
  try{
   const relative=decodeURIComponent(attachment.url.split('/screenshot/')[1].split('?')[0]);
   const source=image(relative);if(!source)continue;
   const digest=crypto.createHash('sha256').update(relative).digest('hex').slice(0,8);
   const destination=path.join(directory,'capture-'+digest+'-'+source.name);
   // Avoid following any preexisting symlink in the import directory.
   if(fs.existsSync(destination)&&fs.lstatSync(destination).isSymbolicLink())continue;
   fs.copyFileSync(source.file,destination);
   const now=new Date();fs.utimesSync(destination,now,now);
  }catch{}
 }
}
module.exports={image,list,importReferences};
